// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>

namespace Common {

/// Debug switches for bisecting the AMD device loss: EDEN_NX2_<name>=0 turns the feature off.
/// Unset, or any other value, keeps it on (the upstream behaviour).
inline bool Nx2Switch(const char* name) {
    const char* const value = std::getenv(name);
    return value == nullptr || value[0] != '0';
}

} // namespace Common
