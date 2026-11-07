// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>

namespace VideoCommon::Nx2Perf {

// Read once at startup: Verify() runs on hot paths, where a function-local static costs a
// thread-safe initialization check on every call.
inline const bool g_verify = [] {
    const char* value = std::getenv("EDEN_NX2_PERF_VERIFY");
    return value != nullptr && value[0] == '1';
}();

inline bool Verify() {
    return g_verify;
}

} // namespace VideoCommon::Nx2Perf
