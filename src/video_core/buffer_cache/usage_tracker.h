// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <vector>

#include "common/alignment.h"
#include "common/common_types.h"

namespace VideoCommon {

class UsageTracker {
    // PAGE_SHIFT is a macro on FreeBSD
    static constexpr size_t BUFFER_BYTES_PER_BITSHIFT = 6;
    static constexpr size_t BUFFER_PAGE_SHIFT = 6 + BUFFER_BYTES_PER_BITSHIFT;
    static constexpr size_t BUFFER_PAGE_BYTES = 1 << BUFFER_PAGE_SHIFT;
public:
    explicit UsageTracker(size_t size) {
        const size_t num_pages = (size >> BUFFER_PAGE_SHIFT) + 1;
        pages.resize(num_pages, 0ULL);
    }

    void Reset() noexcept {
        std::ranges::fill(pages, 0ULL);
    }

    /// Marks the 64-byte units overlapping the byte range [offset, offset + size) as used.
    void Track(u64 offset, u64 size) noexcept {
        ForEachPageMask(offset, size, [this](size_t page, u64 mask) {
            pages[page] |= mask;
            return false;
        });
    }

    /// Returns true if any 64-byte unit overlapping [offset, offset + size) was tracked. Rounds
    /// outward like Track, so it never misses a tracked byte in the queried range.
    [[nodiscard]] bool IsUsed(u64 offset, u64 size) const noexcept {
        bool used = false;
        ForEachPageMask(offset, size, [&](size_t page, u64 mask) {
            used = (pages[page] & mask) != 0;
            return used;
        });
        return used;
    }

private:
    static constexpr u64 UNITS_PER_PAGE_MASK = (1ULL << (BUFFER_PAGE_SHIFT - BUFFER_BYTES_PER_BITSHIFT)) - 1;

    /// Bits [first, last] of a word, both inclusive and in [0, 63].
    static constexpr u64 BitRange(u64 first, u64 last) noexcept {
        return (~u64{0} >> (63 - last)) & (~u64{0} << first);
    }

    /// Calls func(page, mask) for each page touched by [offset, offset + size), clamped to the
    /// tracked size, until func returns true.
    template <typename Func>
    void ForEachPageMask(u64 offset, u64 size, Func&& func) const noexcept {
        if (size == 0 || pages.empty()) {
            return;
        }
        const u64 max_unit = (u64(pages.size()) << (BUFFER_PAGE_SHIFT - BUFFER_BYTES_PER_BITSHIFT)) - 1;
        const u64 first_unit = offset >> BUFFER_BYTES_PER_BITSHIFT;
        if (first_unit > max_unit) {
            return;
        }
        const u64 end = offset + size < offset ? ~u64{0} : offset + size; // saturate on overflow
        const u64 last_unit = std::min<u64>((end - 1) >> BUFFER_BYTES_PER_BITSHIFT, max_unit);
        const u64 first_page = first_unit >> (BUFFER_PAGE_SHIFT - BUFFER_BYTES_PER_BITSHIFT);
        const u64 last_page = last_unit >> (BUFFER_PAGE_SHIFT - BUFFER_BYTES_PER_BITSHIFT);
        for (u64 page = first_page; page <= last_page; ++page) {
            const u64 lo = page == first_page ? (first_unit & UNITS_PER_PAGE_MASK) : 0;
            const u64 hi = page == last_page ? (last_unit & UNITS_PER_PAGE_MASK) : 63;
            if (func(size_t(page), BitRange(lo, hi))) {
                return;
            }
        }
    }

private:
    std::vector<u64> pages;
};

} // namespace VideoCommon
