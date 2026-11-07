// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Copies of GPU-cached guest pages as they were before a guest core wrote them, for the GPU
// thread while it still processes the frame the game already presented.
//
// Running a frame ahead, the game writes the next frame's data (Ocarina of Time's HUD) into
// memory the presented frame's queued draws still read. Waiting at the write (the frame guard)
// kept the game from overlapping the GPU thread: ~12-17 ms of every 37 ms frame. Instead, the
// first write to a page while the frame is pending saves the page; the GPU thread reads the
// saved bytes until it reaches the frame's fence, then the pages are released and invalidated so
// the next frame uploads what the game wrote. EDEN_NX2_WRITE_SNAPSHOT=0 waits as before.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "common/nx2_timeline.h"

namespace Core::Nx2Snapshot {

constexpr u64 PageBits = 12;
constexpr u64 PageSize = u64{1} << PageBits;

struct Page {
    std::array<u8, PageSize> bytes;
    u32 syncpoint;
    u32 value;
};

/// One bit per saved device page (36-bit device addresses), so the reader's many reads of pages
/// without a copy skip the lock and the map.
constexpr u64 BitmapPages = u64{1} << (36 - PageBits);

struct State {
    std::mutex mutex;
    std::unordered_map<u64, std::unique_ptr<Page>> pages; ///< Keyed by device page
    std::atomic<u32> count{};
    std::array<std::atomic<u64>, BitmapPages / 64> saved{};
};

inline void MarkSaved(State& state, u64 page, bool saved) {
    if (page < BitmapPages) {
        const u64 bit = u64{1} << (page % 64);
        if (saved) {
            state.saved[page / 64].fetch_or(bit, std::memory_order_release);
        } else {
            state.saved[page / 64].fetch_and(~bit, std::memory_order_release);
        }
    }
}

/// Whether any page of [address, address + size) may have a saved copy (without the lock).
[[nodiscard]] inline bool MaySave(State& state, u64 address, u64 size) {
    for (u64 page = address >> PageBits; page <= (address + size - 1) >> PageBits; ++page) {
        if (page >= BitmapPages ||
            ((state.saved[page / 64].load(std::memory_order_acquire) >> (page % 64)) & 1) != 0) {
            return true;
        }
    }
    return false;
}

inline State& Get() {
    static State state;
    return state;
}

/// Set on the thread whose reads see the saved pages (the GPU thread's back).
inline thread_local bool t_reader = false;

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_WRITE_SNAPSHOT");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

/// Guest core, before writing: saves the device page if it has no copy for this frame yet.
inline void Capture(u64 device_page, const u8* page_pointer, u32 syncpoint, u32 value) {
    if (page_pointer == nullptr) {
        return;
    }
    const Common::Nx2Timeline::Span timeline_span{Common::Nx2Timeline::Kind::Mark, 1};
    State& state = Get();
    std::scoped_lock lk{state.mutex};
    auto& slot = state.pages[device_page];
    if (slot && slot->syncpoint == syncpoint && slot->value == value) {
        return; // Saved for this frame already: keep the bytes from before its first write.
    }
    if (!slot) {
        slot = std::make_unique<Page>();
        state.count.fetch_add(1, std::memory_order_release);
        MarkSaved(state, device_page, true);
    }
    std::memcpy(slot->bytes.data(), page_pointer, PageSize);
    slot->syncpoint = syncpoint;
    slot->value = value;
}

[[nodiscard]] inline bool Active() {
    return t_reader && Get().count.load(std::memory_order_acquire) != 0;
}

/// Whether a saved page overlaps [address, address + size) (reads that use host pointers
/// directly must copy instead).
[[nodiscard]] inline bool Overlaps(u64 address, u64 size) {
    if (!Active() || size == 0) {
        return false;
    }
    State& state = Get();
    if (!MaySave(state, address, size)) {
        return false;
    }
    std::scoped_lock lk{state.mutex};
    for (u64 page = address >> PageBits; page <= (address + size - 1) >> PageBits; ++page) {
        if (state.pages.contains(page)) {
            return true;
        }
    }
    return false;
}

/// The reader, after copying [address, address + size) from guest memory into dest: puts the
/// saved bytes over it.
inline void Overlay(u64 address, u8* dest, u64 size) {
    if (!Active() || size == 0) {
        return;
    }
    State& state = Get();
    if (!MaySave(state, address, size)) {
        return;
    }
    std::scoped_lock lk{state.mutex};
    const u64 end = address + size;
    for (u64 page = address >> PageBits; page <= (end - 1) >> PageBits; ++page) {
        const auto it = state.pages.find(page);
        if (it == state.pages.end()) {
            continue;
        }
        const u64 page_begin = page << PageBits;
        const u64 begin = (std::max)(address, page_begin);
        const u64 stop = (std::min)(end, page_begin + PageSize);
        std::memcpy(dest + (begin - address), it->second->bytes.data() + (begin - page_begin),
                    stop - begin);
    }
}

/// The reader wrote [address, address + size): keep saved pages in step with its own writes.
inline void WriteThrough(u64 address, const u8* src, u64 size) {
    if (!Active() || size == 0) {
        return;
    }
    State& state = Get();
    if (!MaySave(state, address, size)) {
        return;
    }
    std::scoped_lock lk{state.mutex};
    const u64 end = address + size;
    for (u64 page = address >> PageBits; page <= (end - 1) >> PageBits; ++page) {
        const auto it = state.pages.find(page);
        if (it == state.pages.end()) {
            continue;
        }
        const u64 page_begin = page << PageBits;
        const u64 begin = (std::max)(address, page_begin);
        const u64 stop = (std::min)(end, page_begin + PageSize);
        std::memcpy(it->second->bytes.data() + (begin - page_begin), src + (begin - address),
                    stop - begin);
    }
}

/// The reader: drops the pages whose frame the GPU thread has processed and returns their
/// device addresses (to invalidate, so the caches upload what the game wrote).
template <typename IsReached>
std::vector<u64> Release(IsReached&& is_reached) {
    std::vector<u64> released;
    State& state = Get();
    if (state.count.load(std::memory_order_acquire) == 0) {
        return released;
    }
    std::scoped_lock lk{state.mutex};
    // Pages are saved per presented frame: ask once per frame, not once per page.
    u64 last_key = ~u64{0};
    bool last_reached = false;
    for (auto it = state.pages.begin(); it != state.pages.end();) {
        const u64 key = (u64{it->second->syncpoint} << 32) | it->second->value;
        if (key != last_key) {
            last_key = key;
            last_reached = is_reached(it->second->syncpoint, it->second->value);
        }
        if (last_reached) {
            released.push_back(it->first << PageBits);
            MarkSaved(state, it->first, false);
            it = state.pages.erase(it);
            state.count.fetch_sub(1, std::memory_order_release);
        } else {
            ++it;
        }
    }
    return released;
}

} // namespace Core::Nx2Snapshot
