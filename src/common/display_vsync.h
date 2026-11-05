// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

#include "common/common_types.h"

/// The host display's refresh timing, published by the presentation backend
/// (VK_GOOGLE_display_timing) so the emulated vsync can lock its phase to it.
namespace Common::DisplayVsync {

/// CLOCK_MONOTONIC time of the latest known display refresh (0 if unknown).
inline std::atomic<s64> last_refresh_ns{0};
/// Display refresh period (0 if unknown).
inline std::atomic<s64> refresh_period_ns{0};

} // namespace Common::DisplayVsync
