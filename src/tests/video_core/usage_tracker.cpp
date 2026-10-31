// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <random>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/buffer_cache/usage_tracker.h"

using VideoCommon::UsageTracker;

TEST_CASE("UsageTracker: disjoint ranges in one page do not overlap", "[video_core]") {
    UsageTracker tracker{8192};
    tracker.Track(0, 64);
    REQUIRE(tracker.IsUsed(0, 64));
    REQUIRE(!tracker.IsUsed(128, 64));
    REQUIRE(!tracker.IsUsed(4096, 64));
}

TEST_CASE("UsageTracker: short and unaligned ranges round outward", "[video_core]") {
    UsageTracker tracker{8192};
    tracker.Track(0, 1);
    REQUIRE(tracker.IsUsed(0, 1));
    REQUIRE(tracker.IsUsed(63, 1));
    REQUIRE(!tracker.IsUsed(64, 1));
    tracker.Track(4064, 64); // straddles the 4096-byte page boundary
    REQUIRE(tracker.IsUsed(4095, 1));
    REQUIRE(tracker.IsUsed(4096, 1));
    REQUIRE(tracker.IsUsed(4159, 1));
    REQUIRE(!tracker.IsUsed(4160, 64));
    REQUIRE(!tracker.IsUsed(100, 0));
}

TEST_CASE("UsageTracker: matches a 64-byte unit reference model", "[video_core]") {
    std::mt19937_64 rng{7};
    const std::vector<u64> edges{0, 1, 63, 64, 65, 4095, 4096, 4097, 8191, 8192, 12288};
    const auto pick = [&](u64 limit) {
        return rng() % 2 ? edges[rng() % edges.size()] : rng() % limit;
    };
    for (int iteration = 0; iteration < 500; ++iteration) {
        constexpr u64 size = 16384;
        UsageTracker tracker{size};
        std::vector<bool> units(((size >> 12) + 1) * 64);
        const auto for_units = [&](u64 offset, u64 length, auto&& func) {
            for (u64 unit = offset >> 6; length != 0 && unit <= (offset + length - 1) >> 6 && unit < units.size(); ++unit) {
                func(unit);
            }
        };
        for (int i = 0; i < 4; ++i) {
            const u64 offset = pick(size), length = pick(9000);
            tracker.Track(offset, length);
            for_units(offset, length, [&](u64 unit) { units[unit] = true; });
        }
        for (int i = 0; i < 32; ++i) {
            const u64 offset = pick(size), length = pick(9000);
            bool expected = false;
            for_units(offset, length, [&](u64 unit) { expected = expected || units[unit]; });
            REQUIRE(tracker.IsUsed(offset, length) == expected);
        }
    }
}

TEST_CASE("UsageTracker: reset clears tracked usage", "[video_core]") {
    UsageTracker tracker{8192};
    tracker.Reset();
    REQUIRE(!tracker.IsUsed(0, 8192));
    tracker.Track(100, 5000);
    REQUIRE(tracker.IsUsed(4096, 1));
    tracker.Reset();
    REQUIRE(!tracker.IsUsed(0, 8192));
    tracker.Reset();
    tracker.Track(8000, 10);
    REQUIRE(tracker.IsUsed(8000, 1));
    REQUIRE(!tracker.IsUsed(0, 7936));
}
