// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "common/common_types.h"
#include "common/logging.h"

#if defined(ARCHITECTURE_x86_64)
#include "dynarmic/backend/x64/perf_map.h"
#endif

namespace Core::Nx2BlockCounts {

// EDEN_NX2_BLOCK_COUNTS=<file>: counts how often each guest block of the main module runs
// between EDEN_NX2_BLOCK_COUNTS_START (default 60) and +EDEN_NX2_BLOCK_COUNTS_SECONDS (default
// 10) seconds after the module loads, and writes "offset_in_main count" lines (to find the code
// that repeats a resource load, and which branch decides it).
inline void OnMainLoaded(u64 base, u64 size) {
#if defined(ARCHITECTURE_x86_64)
    const char* path = std::getenv("EDEN_NX2_BLOCK_COUNTS");
    if (path == nullptr || *path == '\0') {
        return;
    }
    auto& counters = Dynarmic::Backend::X64::GetBlockCounters();
    if (counters.counters != nullptr) {
        return;
    }
    const u64 count = size / 4;
    counters.counters = new u32[count]{};
    counters.lo = base;
    counters.count = count;
    LOG_INFO(Loader, "nx2 block counts for main at {:#x} ({:#x} bytes) -> {}", base, size, path);
    const auto env_seconds = [](const char* name, double fallback) {
        const char* text = std::getenv(name);
        return text != nullptr ? std::strtod(text, nullptr) : fallback;
    };
    std::thread{[out = std::string{path}, start = env_seconds("EDEN_NX2_BLOCK_COUNTS_START", 60.0),
                 seconds = env_seconds("EDEN_NX2_BLOCK_COUNTS_SECONDS", 10.0)] {
        auto& c = Dynarmic::Backend::X64::GetBlockCounters();
        std::this_thread::sleep_for(std::chrono::duration<double>(start));
        std::vector<u32> before(c.counters, c.counters + c.count);
        std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
        if (std::FILE* file = std::fopen(out.c_str(), "w")) {
            std::fprintf(file, "# main base %llx, %.1f s\n", static_cast<unsigned long long>(c.lo),
                         seconds);
            for (u64 i = 0; i < c.count; ++i) {
                const u32 runs = c.counters[i] - before[i];
                if (runs != 0) {
                    std::fprintf(file, "%llx %u\n", static_cast<unsigned long long>(i * 4), runs);
                }
            }
            std::fclose(file);
        }
        LOG_INFO(Loader, "nx2 block counts written to {}", out);
    }}.detach();
#else
    (void)base;
    (void)size;
#endif
}

} // namespace Core::Nx2BlockCounts
