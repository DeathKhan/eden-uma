// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/sampler_lookup_cache.h"

using VideoCommon::SamplerId;
using VideoCommon::SamplerLookupCache;

TEST_CASE("SamplerLookupCache reuses consecutive indices", "[video_core]") {
    SamplerLookupCache cache;
    u32 calls{};
    const auto lookup = [&](u32 index) {
        ++calls;
        return SamplerId{index + 100};
    };

    REQUIRE(cache.Get(7, lookup) == SamplerId{107});
    REQUIRE(cache.Get(7, lookup) == SamplerId{107});
    REQUIRE(cache.Get(7, lookup) == SamplerId{107});
    REQUIRE(calls == 1);

    REQUIRE(cache.Get(8, lookup) == SamplerId{108});
    REQUIRE(calls == 2);
    REQUIRE(cache.Get(7, lookup) == SamplerId{107});
    REQUIRE(calls == 3);
}

TEST_CASE("SamplerLookupCache does not retain results across draws", "[video_core]") {
    SamplerId current_id{12};
    u32 calls{};
    const auto lookup = [&](u32) {
        ++calls;
        return current_id;
    };
    {
        SamplerLookupCache draw;
        REQUIRE(draw.Get(3, lookup) == SamplerId{12});
        REQUIRE(draw.Get(3, lookup) == SamplerId{12});
    }
    current_id = SamplerId{24};
    {
        SamplerLookupCache next_draw;
        REQUIRE(next_draw.Get(3, lookup) == SamplerId{24});
        REQUIRE(next_draw.Get(3, lookup) == SamplerId{24});
    }
    REQUIRE(calls == 2);
}

TEST_CASE("SamplerLookupCache keeps graphics and compute results separate", "[video_core]") {
    SamplerLookupCache graphics;
    SamplerLookupCache compute;
    REQUIRE(graphics.Get(5, [](u32) { return SamplerId{10}; }) == SamplerId{10});
    REQUIRE(compute.Get(5, [](u32) { return SamplerId{20}; }) == SamplerId{20});
}

TEST_CASE("SamplerLookupCache retains null sampler results", "[video_core]") {
    SamplerLookupCache cache;
    u32 calls{};
    const auto lookup = [&](u32 index) {
        ++calls;
        return index == 100 ? VideoCommon::NULL_SAMPLER_ID : SamplerId{42};
    };
    REQUIRE(cache.Get(100, lookup) == VideoCommon::NULL_SAMPLER_ID);
    REQUIRE(cache.Get(100, lookup) == VideoCommon::NULL_SAMPLER_ID);
    REQUIRE(calls == 1);
    REQUIRE(cache.Get(0, lookup) == SamplerId{42});
    REQUIRE(calls == 2);
}

TEST_CASE("SamplerLookupCache accepts every index without a sentinel", "[video_core]") {
    SamplerLookupCache cache;
    u32 calls{};
    const auto lookup = [&](u32 index) {
        ++calls;
        REQUIRE(index == std::numeric_limits<u32>::max());
        return SamplerId{42};
    };
    REQUIRE(cache.Get(std::numeric_limits<u32>::max(), lookup) == SamplerId{42});
    REQUIRE(cache.Get(std::numeric_limits<u32>::max(), lookup) == SamplerId{42});
    REQUIRE(calls == 1);
}
