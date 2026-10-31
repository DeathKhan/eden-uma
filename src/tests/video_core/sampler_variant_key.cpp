// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <bit>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/vk_sampler_variant_key.h"

namespace {

using Key = Vulkan::SamplerVariantKey;
constexpr std::array FLAGS{Key::ReduceAnisotropy, Key::ForceNearest, Key::DropReduction,
                           Key::DropCustomBorder, Key::SrgbBorder};
constexpr std::array SWIZZLES{VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_ZERO,
                              VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_R,
                              VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B,
                              VK_COMPONENT_SWIZZLE_A};

Key MakeKey(u32 flags, const VkComponentMapping& mapping = {}) {
    Key key;
    for (const auto flag : FLAGS) {
        key.Set(flag, (flags & flag) != 0);
    }
    key.SetSwizzle(mapping);
    return key;
}

} // namespace

TEST_CASE("SamplerVariantKey preserves every legal flag and swizzle combination", "[video_core]") {
    std::vector<bool> seen(1U << 17);
    u32 combinations{};
    for (u32 flags = 0; flags < 32; ++flags) {
        for (const auto r : SWIZZLES) {
            for (const auto g : SWIZZLES) {
                for (const auto b : SWIZZLES) {
                    for (const auto a : SWIZZLES) {
                        const VkComponentMapping mapping{r, g, b, a};
                        const auto key = MakeKey(flags, mapping);
                        const auto unpacked = key.Swizzle();
                        REQUIRE(unpacked.r == r);
                        REQUIRE(unpacked.g == g);
                        REQUIRE(unpacked.b == b);
                        REQUIRE(unpacked.a == a);
                        for (const auto flag : FLAGS) {
                            REQUIRE(key.Test(flag) == ((flags & flag) != 0));
                        }
                        REQUIRE(key.HasSwizzle() == (r != 0 || g != 0 || b != 0 || a != 0));
                        REQUIRE(key == MakeKey(flags, mapping));

                        const u32 encoded = std::bit_cast<u32>(key);
                        REQUIRE(encoded < seen.size());
                        REQUIRE_FALSE(seen[encoded]);
                        seen[encoded] = true;
                        ++combinations;

                        auto budget_fallback = key;
                        budget_fallback.Set(Key::DropCustomBorder, true);
                        budget_fallback.ClearSwizzle();
                        REQUIRE(budget_fallback == MakeKey(flags | Key::DropCustomBorder));

                        auto capped_variant = key;
                        capped_variant.Set(Key::SrgbBorder, false);
                        capped_variant.ClearSwizzle();
                        REQUIRE(capped_variant == MakeKey(flags & ~Key::SrgbBorder));
                    }
                }
            }
        }
    }
    REQUIRE(combinations == 76832);
}

TEST_CASE("SamplerVariantKey keeps identity distinct from explicit component selectors",
          "[video_core]") {
    const Key identity;
    const auto explicit_rgba = MakeKey(0, {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G,
                                         VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A});
    REQUIRE_FALSE(identity.HasSwizzle());
    REQUIRE(explicit_rgba.HasSwizzle());
    REQUIRE_FALSE(identity == explicit_rgba);
}

TEST_CASE("SamplerVariantKey updates flags independently of swizzles", "[video_core]") {
    const VkComponentMapping mapping{VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_B,
                                     VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R};
    for (const auto flag : FLAGS) {
        auto key = MakeKey(0, mapping);
        key.Set(flag, true);
        REQUIRE(key == MakeKey(flag, mapping));
        key.Set(flag, false);
        REQUIRE(key == MakeKey(0, mapping));
    }
}
