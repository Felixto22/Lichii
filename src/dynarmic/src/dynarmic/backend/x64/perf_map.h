// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include <bit>

namespace Dynarmic::Backend::X64 {

// A map of the emitted code: Linux perf's (PERF_BUILDID_DIR) or, with EDEN_NX2_JIT_MAP=<file>,
// one on any system that also lists each block's guest instructions, so a sampling profiler's
// JIT addresses can be traced back to guest code (Nx2Sampler writes <out>.jit).
namespace detail {
bool PerfMapEnabled() noexcept;
void PerfMapRegister(const void* start, const void* end, std::string_view friendly_name);
}  // namespace detail
template<typename T>
void PerfMapRegister(T start, const void* end, std::string_view friendly_name) noexcept {
    if (detail::PerfMapEnabled()) {
        detail::PerfMapRegister(std::bit_cast<const void*>(start), end, friendly_name);
    }
}
inline bool PerfMapEnabled() noexcept {
    return detail::PerfMapEnabled();
}
/// The guest instruction words of the block emitted at `start` (EDEN_NX2_JIT_MAP only).
void PerfMapBlockCode(const void* start, std::uint64_t pc, const std::uint32_t* words, std::size_t count);
void PerfMapClear();

}  // namespace Dynarmic::Backend::X64
