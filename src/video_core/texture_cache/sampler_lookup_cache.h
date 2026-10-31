// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "video_core/texture_cache/types.h"

namespace VideoCommon {

// Keep this cache local to one draw or dispatch, after SynchronizeDescriptors.
class SamplerLookupCache {
public:
    template <typename Lookup>
    SamplerId Get(u32 index, Lookup&& lookup) {
        if (!valid || index != cached_index) {
            cached_id = lookup(index);
            cached_index = index;
            valid = true;
        }
        return cached_id;
    }

private:
    u32 cached_index{};
    SamplerId cached_id{};
    bool valid{};
};

} // namespace VideoCommon
