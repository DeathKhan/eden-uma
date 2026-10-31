// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cmath>
#include <cstring>
#include <span>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/bit_util.h"
#include "common/div_ceil.h"
#include "video_core/gpu.h"
#include "video_core/textures/decoders.h"

namespace Tegra::Texture {
namespace {
template <u32 mask>
constexpr u32 pdep(u32 value) {
    u32 result = 0;
    u32 m = mask;
    for (u32 bit = 1; m; bit += bit) {
        if (value & bit)
            result |= m & (~m + 1);
        m &= m - 1;
    }
    return result;
}

template <u32 mask, u32 incr_amount>
void incrpdep(u32& value) {
    static constexpr u32 swizzled_incr = pdep<mask>(incr_amount);
    value = ((value | ~mask) + swizzled_incr) & mask;
}

// Within a 64-byte GOB row, linear byte bits 0-3 map to address bits 0-3 unchanged, bit 4 to bit 5
// and bit 5 to bit 8. Every 16-byte aligned run of a row is therefore contiguous in the swizzled
// layout, at these offsets from the start of its GOB row.
constexpr std::array<u32, 4> SWIZZLED_16B_RUN{
    pdep<SWIZZLE_X_BITS>(0), pdep<SWIZZLE_X_BITS>(16), pdep<SWIZZLE_X_BITS>(32),
    pdep<SWIZZLE_X_BITS>(48)};

/// Copies the 16-byte aligned runs of one row in [x_begin, x_end) (absolute linear byte offsets,
/// both multiples of 16) instead of going texel by texel.
template <bool TO_LINEAR>
void SwizzleRow16(u8* output, const u8* input, u32 x_begin, u32 x_end, u32 row_swizzled,
                  u32 row_linear, u32 x_shift) {
    for (u32 x = x_begin; x < x_end; x += 16) {
        const u32 swizzled_offset =
            row_swizzled + ((x >> GOB_SIZE_X_SHIFT) << x_shift) + SWIZZLED_16B_RUN[(x >> 4) & 3];
        const u32 unswizzled_offset = row_linear + (x - x_begin);
        u8* const dst = output + (TO_LINEAR ? swizzled_offset : unswizzled_offset);
        const u8* const src = input + (TO_LINEAR ? unswizzled_offset : swizzled_offset);
        std::memcpy(dst, src, 16);
    }
}

template <bool TO_LINEAR, u32 BYTES_PER_PIXEL>
void SwizzleImpl(std::span<u8> output, std::span<const u8> input, u32 width, u32 height, u32 depth,
                 u32 block_height, u32 block_depth, u32 stride) {
    // The origin of the transformation can be configured here, leave it as zero as the current API
    // doesn't expose it.
    static constexpr u32 origin_x = 0;
    static constexpr u32 origin_y = 0;
    static constexpr u32 origin_z = 0;

    // We can configure here a custom pitch
    // As it's not exposed 'width * BYTES_PER_PIXEL' will be the expected pitch.
    const u32 pitch = width * BYTES_PER_PIXEL;

    const u32 gobs_in_x = Common::DivCeilLog2(stride, GOB_SIZE_X_SHIFT);
    const u32 block_size = gobs_in_x << (GOB_SIZE_SHIFT + block_height + block_depth);
    const u32 slice_size =
        Common::DivCeilLog2(height, block_height + GOB_SIZE_Y_SHIFT) * block_size;

    const u32 block_height_mask = (1U << block_height) - 1;
    const u32 block_depth_mask = (1U << block_depth) - 1;
    const u32 x_shift = GOB_SIZE_SHIFT + block_height + block_depth;

    for (u32 slice = 0; slice < depth; ++slice) {
        const u32 z = slice + origin_z;
        const u32 offset_z = (z >> block_depth) * slice_size +
                             ((z & block_depth_mask) << (GOB_SIZE_SHIFT + block_height));
        for (u32 line = 0; line < height; ++line) {
            const u32 y = line + origin_y;
            const u32 swizzled_y = pdep<SWIZZLE_Y_BITS>(y);

            const u32 block_y = y >> GOB_SIZE_Y_SHIFT;
            const u32 offset_y = (block_y >> block_height) * block_size +
                                 ((block_y & block_height_mask) << GOB_SIZE_SHIFT);

            u32 column = 0;
            u32 swizzled_x = pdep<SWIZZLE_X_BITS>(origin_x * BYTES_PER_PIXEL);
            if constexpr (origin_x == 0 && 16 % BYTES_PER_PIXEL == 0) {
                const u32 run_end = pitch & ~15U;
                SwizzleRow16<TO_LINEAR>(output.data(), input.data(), 0, run_end,
                                        offset_z + offset_y + swizzled_y,
                                        slice * pitch * height + line * pitch, x_shift);
                column = run_end / BYTES_PER_PIXEL;
                swizzled_x = pdep<SWIZZLE_X_BITS>(run_end);
            }
            for (; column < width;
                 ++column, incrpdep<SWIZZLE_X_BITS, BYTES_PER_PIXEL>(swizzled_x)) {
                const u32 x = (column + origin_x) * BYTES_PER_PIXEL;
                const u32 offset_x = (x >> GOB_SIZE_X_SHIFT) << x_shift;

                const u32 base_swizzled_offset = offset_z + offset_y + offset_x;
                const u32 swizzled_offset = base_swizzled_offset + (swizzled_x | swizzled_y);

                const u32 unswizzled_offset =
                    slice * pitch * height + line * pitch + column * BYTES_PER_PIXEL;

                u8* const dst = &output[TO_LINEAR ? swizzled_offset : unswizzled_offset];
                const u8* const src = &input[TO_LINEAR ? unswizzled_offset : swizzled_offset];

                std::memcpy(dst, src, BYTES_PER_PIXEL);
            }
        }
    }
}

template <bool TO_LINEAR, u32 BYTES_PER_PIXEL>
void SwizzleSubrectImpl(std::span<u8> output, std::span<const u8> input, u32 width, u32 height,
                        u32 depth, u32 origin_x, u32 origin_y, u32 extent_x, u32 num_lines,
                        u32 block_height, u32 block_depth, u32 pitch_linear) {
    // The origin of the transformation can be configured here, leave it as zero as the current API
    // doesn't expose it.
    static constexpr u32 origin_z = 0;

    // We can configure here a custom pitch
    // As it's not exposed 'width * BYTES_PER_PIXEL' will be the expected pitch.
    const u32 pitch = pitch_linear;
    const u32 stride = Common::AlignUpLog2(width * BYTES_PER_PIXEL, GOB_SIZE_X_SHIFT);

    const u32 gobs_in_x = Common::DivCeilLog2(stride, GOB_SIZE_X_SHIFT);
    const u32 block_size = gobs_in_x << (GOB_SIZE_SHIFT + block_height + block_depth);
    const u32 slice_size =
        Common::DivCeilLog2(height, block_height + GOB_SIZE_Y_SHIFT) * block_size;

    const u32 block_height_mask = (1U << block_height) - 1;
    const u32 block_depth_mask = (1U << block_depth) - 1;
    const u32 x_shift = GOB_SIZE_SHIFT + block_height + block_depth;

    u32 unprocessed_lines = num_lines;
    u32 extent_y = (std::min)(num_lines, height - origin_y);

    for (u32 slice = 0; slice < depth; ++slice) {
        const u32 z = slice + origin_z;
        const u32 offset_z = (z >> block_depth) * slice_size +
                             ((z & block_depth_mask) << (GOB_SIZE_SHIFT + block_height));
        const u32 lines_in_y = (std::min)(unprocessed_lines, extent_y);
        for (u32 line = 0; line < lines_in_y; ++line) {
            const u32 y = line + origin_y;
            const u32 swizzled_y = pdep<SWIZZLE_Y_BITS>(y);

            const u32 block_y = y >> GOB_SIZE_Y_SHIFT;
            const u32 offset_y = (block_y >> block_height) * block_size +
                                 ((block_y & block_height_mask) << GOB_SIZE_SHIFT);

            u32 column = 0;
            u32 swizzled_x = pdep<SWIZZLE_X_BITS>(origin_x * BYTES_PER_PIXEL);
            const auto copy_texels = [&](u32 end_column) {
                for (; column < end_column;
                     ++column, incrpdep<SWIZZLE_X_BITS, BYTES_PER_PIXEL>(swizzled_x)) {
                    const u32 x = (column + origin_x) * BYTES_PER_PIXEL;
                    const u32 offset_x = (x >> GOB_SIZE_X_SHIFT) << x_shift;

                    const u32 base_swizzled_offset = offset_z + offset_y + offset_x;
                    const u32 swizzled_offset = base_swizzled_offset + (swizzled_x | swizzled_y);

                    const u32 unswizzled_offset =
                        slice * pitch * height + line * pitch + column * BYTES_PER_PIXEL;

                    u8* const dst = &output[TO_LINEAR ? swizzled_offset : unswizzled_offset];
                    const u8* const src = &input[TO_LINEAR ? unswizzled_offset : swizzled_offset];

                    std::memcpy(dst, src, BYTES_PER_PIXEL);
                }
            };
            if constexpr (16 % BYTES_PER_PIXEL == 0) {
                // Unaligned head texel by texel, then whole 16-byte runs, then the tail.
                const u32 x_begin = origin_x * BYTES_PER_PIXEL;
                const u32 x_end = (origin_x + extent_x) * BYTES_PER_PIXEL;
                const u32 run_begin = (std::min)(Common::AlignUp(x_begin, 16U), x_end);
                const u32 run_end = run_begin + ((x_end - run_begin) & ~15U);
                copy_texels((run_begin - x_begin) / BYTES_PER_PIXEL);
                SwizzleRow16<TO_LINEAR>(output.data(), input.data(), run_begin, run_end,
                                        offset_z + offset_y + swizzled_y,
                                        slice * pitch * height + line * pitch +
                                            (run_begin - x_begin),
                                        x_shift);
                column = (run_end - x_begin) / BYTES_PER_PIXEL;
                swizzled_x = pdep<SWIZZLE_X_BITS>(run_end);
            }
            copy_texels(extent_x);
        }
        unprocessed_lines -= lines_in_y;
        if (unprocessed_lines == 0) {
            return;
        }
    }
}

template <bool TO_LINEAR>
void Swizzle(std::span<u8> output, std::span<const u8> input, u32 bytes_per_pixel, u32 width,
             u32 height, u32 depth, u32 block_height, u32 block_depth, u32 stride_alignment) {
    switch (bytes_per_pixel) {
#define BPP_CASE(x)                                                                                \
    case x:                                                                                        \
        return SwizzleImpl<TO_LINEAR, x>(output, input, width, height, depth, block_height,        \
                                         block_depth, stride_alignment);
        BPP_CASE(1)
        BPP_CASE(2)
        BPP_CASE(3)
        BPP_CASE(4)
        BPP_CASE(6)
        BPP_CASE(8)
        BPP_CASE(12)
        BPP_CASE(16)
#undef BPP_CASE
    default:
        ASSERT_MSG(false, "Invalid bytes_per_pixel={}", bytes_per_pixel);
        break;
    }
}

} // Anonymous namespace

void UnswizzleTexture(std::span<u8> output, std::span<const u8> input, u32 bytes_per_pixel,
                      u32 width, u32 height, u32 depth, u32 block_height, u32 block_depth,
                      u32 stride_alignment) {
    const u32 stride = Common::AlignUpLog2(width, stride_alignment) * bytes_per_pixel;
    const u32 new_bpp = (std::min)(4U, static_cast<u32>(std::countr_zero(width * bytes_per_pixel)));
    width = (width * bytes_per_pixel) >> new_bpp;
    bytes_per_pixel = 1U << new_bpp;
    Swizzle<false>(output, input, bytes_per_pixel, width, height, depth, block_height, block_depth,
                   stride);
}

void SwizzleTexture(std::span<u8> output, std::span<const u8> input, u32 bytes_per_pixel, u32 width,
                    u32 height, u32 depth, u32 block_height, u32 block_depth,
                    u32 stride_alignment) {
    const u32 stride = Common::AlignUpLog2(width, stride_alignment) * bytes_per_pixel;
    const u32 new_bpp = (std::min)(4U, static_cast<u32>(std::countr_zero(width * bytes_per_pixel)));
    width = (width * bytes_per_pixel) >> new_bpp;
    bytes_per_pixel = 1U << new_bpp;
    Swizzle<true>(output, input, bytes_per_pixel, width, height, depth, block_height, block_depth,
                  stride);
}

void SwizzleSubrect(std::span<u8> output, std::span<const u8> input, u32 bytes_per_pixel, u32 width,
                    u32 height, u32 depth, u32 origin_x, u32 origin_y, u32 extent_x, u32 extent_y,
                    u32 block_height, u32 block_depth, u32 pitch_linear) {
    switch (bytes_per_pixel) {
#define BPP_CASE(x)                                                                                \
    case x:                                                                                        \
        return SwizzleSubrectImpl<true, x>(output, input, width, height, depth, origin_x,          \
                                           origin_y, extent_x, extent_y, block_height,             \
                                           block_depth, pitch_linear);
        BPP_CASE(1)
        BPP_CASE(2)
        BPP_CASE(3)
        BPP_CASE(4)
        BPP_CASE(6)
        BPP_CASE(8)
        BPP_CASE(12)
        BPP_CASE(16)
#undef BPP_CASE
    default:
        ASSERT_MSG(false, "Invalid bytes_per_pixel={}", bytes_per_pixel);
        break;
    }
}

void UnswizzleSubrect(std::span<u8> output, std::span<const u8> input, u32 bytes_per_pixel,
                      u32 width, u32 height, u32 depth, u32 origin_x, u32 origin_y, u32 extent_x,
                      u32 extent_y, u32 block_height, u32 block_depth, u32 pitch_linear) {
    switch (bytes_per_pixel) {
#define BPP_CASE(x)                                                                                \
    case x:                                                                                        \
        return SwizzleSubrectImpl<false, x>(output, input, width, height, depth, origin_x,         \
                                            origin_y, extent_x, extent_y, block_height,            \
                                            block_depth, pitch_linear);
        BPP_CASE(1)
        BPP_CASE(2)
        BPP_CASE(3)
        BPP_CASE(4)
        BPP_CASE(6)
        BPP_CASE(8)
        BPP_CASE(12)
        BPP_CASE(16)
#undef BPP_CASE
    default:
        ASSERT_MSG(false, "Invalid bytes_per_pixel={}", bytes_per_pixel);
        break;
    }
}

std::size_t CalculateSize(bool tiled, u32 bytes_per_pixel, u32 width, u32 height, u32 depth,
                          u32 block_height, u32 block_depth) {
    if (tiled) {
        const u32 aligned_width = Common::AlignUpLog2(width * bytes_per_pixel, GOB_SIZE_X_SHIFT);
        const u32 aligned_height = Common::AlignUpLog2(height, GOB_SIZE_Y_SHIFT + block_height);
        const u32 aligned_depth = Common::AlignUpLog2(depth, GOB_SIZE_Z_SHIFT + block_depth);
        return aligned_width * aligned_height * aligned_depth;
    } else {
        return width * height * depth * bytes_per_pixel;
    }
}

u64 GetGOBOffset(u32 width, u32 height, u32 dst_x, u32 dst_y, u32 block_height,
                 u32 bytes_per_pixel) {
    auto div_ceil = [](const u32 x, const u32 y) { return ((x + y - 1) / y); };
    const u32 gobs_in_block = 1 << block_height;
    const u32 y_blocks = GOB_SIZE_Y << block_height;
    const u32 x_per_gob = GOB_SIZE_X / bytes_per_pixel;
    const u32 x_blocks = div_ceil(width, x_per_gob);
    const u32 block_size = GOB_SIZE * gobs_in_block;
    const u32 stride = block_size * x_blocks;
    const u32 base = (dst_y / y_blocks) * stride + (dst_x / x_per_gob) * block_size;
    const u32 relative_y = dst_y % y_blocks;
    return base + (relative_y / GOB_SIZE_Y) * GOB_SIZE;
}

} // namespace Tegra::Texture
