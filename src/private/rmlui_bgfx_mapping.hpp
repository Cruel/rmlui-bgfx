#pragma once

#include "rmlui_bgfx_types.hpp"

namespace rmlui_bgfx {

// Canonical optimized-renderer mapping helpers. Rectangles named Global* are framebuffer-space
// surface coordinates. Rectangles named Local* are relative to the target or texture region origin
// supplied to the helper, not the viewport origin.
[[nodiscard]] inline LocalFbRect local_rect_for_global_rect(GlobalFbRect global_rect,
                                                            GlobalFbRect target_global_bounds)
{
    const GlobalFbRect clipped = intersect(global_rect, target_global_bounds);
    if (is_empty(clipped)) {
        return {};
    }
    return {clipped.x - target_global_bounds.x, clipped.y - target_global_bounds.y, clipped.w,
            clipped.h};
}

[[nodiscard]] inline GlobalFbRect global_rect_for_local_rect(LocalFbRect local_rect,
                                                             GlobalFbRect target_global_bounds)
{
    if (is_empty(local_rect)) {
        return {};
    }
    return {local_rect.x + target_global_bounds.x, local_rect.y + target_global_bounds.y,
            local_rect.w, local_rect.h};
}

[[nodiscard]] inline LocalFbRect texture_local_rect_for_global_rect(const TextureRegion& source,
                                                                    GlobalFbRect sample_global)
{
    const GlobalFbRect clipped = intersect(sample_global, source.global_bounds);
    if (is_empty(clipped)) {
        return {};
    }
    return {source.local_rect.x + clipped.x - source.global_bounds.x,
            source.local_rect.y + clipped.y - source.global_bounds.y, clipped.w, clipped.h};
}

[[nodiscard]] inline TextureRegion texture_subregion_for_global_rect(TextureRegion source,
                                                                     GlobalFbRect sample_global)
{
    const GlobalFbRect clipped = intersect(sample_global, source.global_bounds);
    if (is_empty(clipped)) {
        return {};
    }
    source.local_rect = texture_local_rect_for_global_rect(source, clipped);
    source.global_bounds = clipped;
    return source;
}

} // namespace rmlui_bgfx
