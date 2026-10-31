// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <vulkan/vulkan_core.h>

#include "common/common_types.h"

namespace Vulkan {

class SamplerVariantKey {
public:
    enum Flag : u32 {
        ReduceAnisotropy = 1U << 0,
        ForceNearest = 1U << 1,
        DropReduction = 1U << 2,
        DropCustomBorder = 1U << 3,
        SrgbBorder = 1U << 4,
    };

    constexpr void Set(Flag flag, bool enabled) noexcept {
        value = enabled ? value | flag : value & ~flag;
    }

    [[nodiscard]] constexpr bool Test(Flag flag) const noexcept {
        return (value & flag) != 0;
    }

    constexpr void SetSwizzle(const VkComponentMapping& mapping) noexcept {
        value = (value & FLAGS_MASK) | (static_cast<u32>(mapping.r) << 5) |
                (static_cast<u32>(mapping.g) << 8) | (static_cast<u32>(mapping.b) << 11) |
                (static_cast<u32>(mapping.a) << 14);
    }

    [[nodiscard]] constexpr VkComponentMapping Swizzle() const noexcept {
        return {
            static_cast<VkComponentSwizzle>((value >> 5) & 7),
            static_cast<VkComponentSwizzle>((value >> 8) & 7),
            static_cast<VkComponentSwizzle>((value >> 11) & 7),
            static_cast<VkComponentSwizzle>((value >> 14) & 7),
        };
    }

    constexpr void ClearSwizzle() noexcept {
        value &= FLAGS_MASK;
    }

    [[nodiscard]] constexpr bool HasSwizzle() const noexcept {
        return (value & ~FLAGS_MASK) != 0;
    }

    constexpr bool operator==(const SamplerVariantKey&) const noexcept = default;

private:
    static constexpr u32 FLAGS_MASK = (1U << 5) - 1;
    // Five flags and four three-bit Vulkan swizzles fit in a register-sized key.
    u32 value{};
};

static_assert(VK_COMPONENT_SWIZZLE_IDENTITY == 0);
static_assert(VK_COMPONENT_SWIZZLE_A < (1U << 3));
static_assert(sizeof(SamplerVariantKey) == sizeof(u32));

} // namespace Vulkan
