// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <fmt/format.h>

#include "dynarmic/backend/x64/perf_map.h"
#include "common/common_types.h"
#if defined(__linux__) && !defined(__ANDROID__)
#    include <sys/types.h>
#    include <unistd.h>
#endif

namespace Dynarmic::Backend::X64 {

namespace {
std::mutex mutex;
std::FILE* file = nullptr;
bool opened = false;
bool code_listing = false;

void OpenFile() {
    opened = true;
    if (const char* path = std::getenv("EDEN_NX2_JIT_MAP"); path != nullptr && *path != '\0') {
        file = std::fopen(path, "w");
        code_listing = true;
    } else {
#if defined(__linux__) && !defined(__ANDROID__)
        const char* perf_dir = std::getenv("PERF_BUILDID_DIR");
        if (!perf_dir) {
            return;
        }
        const std::string filename = fmt::format("{:s}/perf-{:d}.map", perf_dir, getpid());
        file = std::fopen(filename.c_str(), "w");
        if (file) {
            std::setvbuf(file, nullptr, _IONBF, 0);
        }
#endif
    }
}
}  // anonymous namespace

namespace detail {
bool PerfMapEnabled() noexcept {
    static const bool enabled = [] {
        const char* map = std::getenv("EDEN_NX2_JIT_MAP");
        if (map != nullptr && *map != '\0') {
            return true;
        }
#if defined(__linux__) && !defined(__ANDROID__)
        return std::getenv("PERF_BUILDID_DIR") != nullptr;
#else
        return false;
#endif
    }();
    return enabled;
}

void PerfMapRegister(const void* start, const void* end, std::string_view friendly_name) {
    if (start == end) {
        // Nothing to register
        return;
    }

    std::lock_guard guard{mutex};

    if (!opened) {
        OpenFile();
    }
    if (!file) {
        return;
    }

    const std::string line = fmt::format("{:016x} {:016x} {:s}\n", reinterpret_cast<u64>(start), reinterpret_cast<u64>(end) - reinterpret_cast<u64>(start), friendly_name);
    std::fwrite(line.data(), sizeof *line.data(), line.size(), file);
}
}  // namespace detail

void PerfMapBlockCode(const void* start, std::uint64_t pc, const std::uint32_t* words, std::size_t count) {
    if (!detail::PerfMapEnabled()) {
        return;
    }
    std::lock_guard guard{mutex};
    if (!opened) {
        OpenFile();
    }
    if (!file || !code_listing) {
        return;
    }
    std::string line = fmt::format("code {:016x} {:016x}", reinterpret_cast<u64>(start), pc);
    for (std::size_t i = 0; i < count; ++i) {
        line += fmt::format(" {:08x}", words[i]);
    }
    line += '\n';
    std::fwrite(line.data(), 1, line.size(), file);
    static unsigned blocks = 0;
    if (++blocks % 512 == 0) {
        std::fflush(file);
    }
}

void PerfMapClear() {
    if (!detail::PerfMapEnabled()) {
        return;
    }
    std::lock_guard guard{mutex};

    if (!file) {
        return;
    }
    if (code_listing) {
        // Keep one file: code emitted later may reuse these addresses (the last entry wins).
        std::fputs("clear\n", file);
        std::fflush(file);
        return;
    }

    std::fclose(file);
    file = nullptr;
    OpenFile();
}

}  // namespace Dynarmic::Backend::X64
