// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// EDEN_NX2_TIMELINE=<file>: records a short timeline of where each host thread waits and works,
// to find the waits on a frame's critical path. From EDEN_NX2_TIMELINE_START seconds after the
// first event (default 75) for EDEN_NX2_TIMELINE_MS milliseconds (default 600), every event
// (GPU profile zones with EDEN_NX2_GPU_PROFILE, split drains, guest core idles, presents, CPU
// reads that flush the GPU) is kept, then written once as CSV:
//   thread,kind,detail,begin_us,end_us

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#ifdef _MSC_VER
#include <intrin.h>
#elif defined(__x86_64__)
#include <x86intrin.h>
#endif

#include "common/common_types.h"

namespace Common::Nx2Timeline {

enum class Kind : u16 {
    Zone,       ///< GPU profile zone (detail: zone index)
    Drain,      ///< Split front waits for the back (detail: drain site)
    GuestIdle,  ///< A guest core has nothing to run
    Present,    ///< nx2:vi2 present (detail: present count low bits)
    CpuRead,    ///< A guest CPU read of GPU-written memory (flushes)
    WriteGuard, ///< A guest write waits for the presented frame
    GpuWait,    ///< Host waits for the host GPU (master semaphore)
    Mark,       ///< Point event (detail: user code)
    BackBusy,   ///< The split's back executes entries
    FrontWait,  ///< The split's front waits for the back (detail: drain site)
};

inline u64 Tsc() {
#if defined(_MSC_VER) || defined(__x86_64__)
    return __rdtsc();
#else
    return static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

struct Event {
    u64 begin;
    u64 end;
    u16 kind;
    u16 thread;
    u32 detail;
};

struct State {
    std::string path;
    bool enabled{};
    double start_s{75.0};
    double length_ms{600.0};
    std::atomic<int> phase{0}; ///< 0 waiting, 1 recording, 2 done
    std::atomic<u64> open_tsc{};
    std::atomic<u64> close_tsc{~u64{0}};
    std::chrono::steady_clock::time_point first{};
    std::chrono::steady_clock::time_point open_time{};
    std::vector<Event> events;
    std::atomic<size_t> count{};
    std::mutex names_mutex;
    std::vector<std::string> names;
    std::atomic<u32> next_thread{};
    std::once_flag init;
};

inline State& Get() {
    static State state;
    std::call_once(state.init, [] {
        if (const char* path = std::getenv("EDEN_NX2_TIMELINE"); path != nullptr && *path != 0) {
            state.path = path;
            state.enabled = true;
            if (const char* s = std::getenv("EDEN_NX2_TIMELINE_START")) {
                state.start_s = std::atof(s);
            }
            if (const char* s = std::getenv("EDEN_NX2_TIMELINE_MS")) {
                state.length_ms = std::atof(s);
            }
            state.events.resize(size_t{4} << 20);
            state.names.resize(256);
            state.first = std::chrono::steady_clock::now();
        }
    });
    return state;
}

inline thread_local int t_thread = -1;
inline thread_local char t_name[32] = "";

inline void SetThreadName(const char* name) {
    std::strncpy(t_name, name, sizeof(t_name) - 1);
}

inline void Dump(State& state);

inline u16 ThreadSlot(State& state) {
    if (t_thread < 0) {
        t_thread = static_cast<int>(state.next_thread.fetch_add(1) & 255);
        std::scoped_lock lk{state.names_mutex};
        state.names[t_thread] = t_name[0] != 0 ? t_name : "thread" + std::to_string(t_thread);
    }
    return static_cast<u16>(t_thread);
}

/// Whether events are being kept now (cheap once the window has passed).
inline bool Recording() {
    State& state = Get();
    if (!state.enabled) [[likely]] {
        return false;
    }
    const int phase = state.phase.load(std::memory_order_acquire);
    if (phase == 2) {
        return false;
    }
    if (phase == 0) {
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - state.first).count() < state.start_s) {
            return false;
        }
        int expected = 0;
        if (state.phase.compare_exchange_strong(expected, 1)) {
            state.open_time = now;
            const u64 tsc = Tsc();
            state.open_tsc.store(tsc);
            // Close after length_ms, measured in TSC ticks once a calibration is known: until
            // then the steady clock decides (Record checks it every 4096 events).
        }
        return true;
    }
    return true;
}

inline void Record(Kind kind, u64 begin, u64 end, u32 detail = 0) {
    if (!Recording()) [[likely]] {
        return;
    }
    State& state = Get();
    if (begin < state.open_tsc.load(std::memory_order_relaxed)) {
        begin = state.open_tsc.load(std::memory_order_relaxed);
        if (end < begin) {
            return;
        }
    }
    const size_t index = state.count.fetch_add(1, std::memory_order_relaxed);
    if (index >= state.events.size() ||
        ((index & 4095) == 0 &&
         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                   state.open_time)
                 .count() > state.length_ms)) {
        int expected = 1;
        if (state.phase.compare_exchange_strong(expected, 2)) {
            Dump(state);
        }
        return;
    }
    state.events[index] = Event{begin, end, static_cast<u16>(kind), ThreadSlot(state), detail};
}

/// Times a span of the calling thread.
class Span {
public:
    explicit Span(Kind kind_, u32 detail_ = 0) : kind{kind_}, detail{detail_} {
        if (Recording()) {
            begin = Tsc();
        }
    }
    ~Span() {
        if (begin != 0) {
            Record(kind, begin, Tsc(), detail);
        }
    }
    Span(const Span&) = delete;
    Span& operator=(const Span&) = delete;

private:
    Kind kind;
    u32 detail;
    u64 begin{};
};

inline void Dump(State& state) {
    const auto close_time = std::chrono::steady_clock::now();
    const u64 close_tsc = Tsc();
    const u64 open_tsc = state.open_tsc.load();
    const double ns = std::chrono::duration<double, std::nano>(close_time - state.open_time).count();
    const double ns_per_tick = close_tsc > open_tsc ? ns / static_cast<double>(close_tsc - open_tsc) : 1.0;
    std::FILE* file = std::fopen(state.path.c_str(), "w");
    if (file == nullptr) {
        return;
    }
    static constexpr const char* kinds[] = {"zone",     "drain",       "guest_idle", "present",
                                            "cpu_read", "write_guard", "gpu_wait",   "mark",
                                            "back_busy", "front_wait"};
    std::fprintf(file, "thread,kind,detail,begin_us,end_us\n");
    const size_t count = (std::min)(state.count.load(), state.events.size());
    std::vector<std::string> names;
    {
        std::scoped_lock lk{state.names_mutex};
        names = state.names;
    }
    for (size_t i = 0; i < count; ++i) {
        const Event& event = state.events[i];
        if (event.end == 0) {
            continue;
        }
        const double begin_us = static_cast<double>(event.begin - open_tsc) * ns_per_tick / 1000.0;
        const double end_us = static_cast<double>(event.end - open_tsc) * ns_per_tick / 1000.0;
        std::fprintf(file, "%s,%s,%u,%.2f,%.2f\n", names[event.thread].c_str(),
                     kinds[(std::min)(static_cast<size_t>(event.kind), size_t{9})], event.detail,
                     begin_us, end_us);
    }
    std::fclose(file);
}

} // namespace Common::Nx2Timeline
