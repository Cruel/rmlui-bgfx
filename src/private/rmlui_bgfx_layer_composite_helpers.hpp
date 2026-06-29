#pragma once

#include "rmlui_bgfx_layers.hpp"
#include "rmlui_bgfx_mapping.hpp"

#include <algorithm>
#include <optional>

namespace rmlui_bgfx {

// The layer texture region preserves a global framebuffer-space rectangle alongside the
// texture-local pixels that represent it. This is the bridge between GL3's full-layer semantics
// and compact bgfx layer targets.
[[nodiscard]] inline TextureRegion make_layer_texture_region(bgfx::TextureHandle texture,
                                                             GlobalFbRect global_bounds,
                                                             LocalFbRect local_rect,
                                                             int texture_width, int texture_height)
{
    return TextureRegion{texture, global_bounds, local_rect, texture_width, texture_height};
}

// `source` carries both global bounds and texture-local sampling bounds. `destination_rect` and
// `scissor` must already be destination-target-local; submit_composite() does not subtract the
// destination layer origin again.
[[nodiscard]] inline CompositeOp
make_layer_composite_op(TextureRegion source, bgfx::FrameBufferHandle destination,
                        Rml::BlendMode blend_mode, ScissorState scissor,
                        bool apply_destination_stencil, uint8_t stencil_ref, RmlUiPassKind kind,
                        RmlUiPassReason reason, const char* name, LocalFbRect destination_rect = {},
                        CompositeFilterState filter = {})
{
    CompositeOp op;
    op.source = source;
    op.destination = destination;
    op.destination_rect = destination_rect;
    op.blend_mode = blend_mode;
    op.scissor = scissor;
    op.apply_destination_stencil = apply_destination_stencil;
    op.stencil_ref = stencil_ref;
    op.kind = kind;
    op.reason = reason;
    op.name = name;
    op.filter = filter;
    return op;
}

[[nodiscard]] inline Rml::Rectanglei clamp_scissor_to_surface(const Rml::Rectanglei& rect,
                                                              const SurfaceMetrics& surface)
{
    const int left = std::clamp(rect.Left(), 0, surface.framebuffer_width);
    const int top = std::clamp(rect.Top(), 0, surface.framebuffer_height);
    const int right = std::clamp(rect.Right(), 0, surface.framebuffer_width);
    const int bottom = std::clamp(rect.Bottom(), 0, surface.framebuffer_height);
    if (right <= left || bottom <= top) {
        return Rml::Rectanglei::FromPositionSize({0, 0}, {0, 0});
    }
    return Rml::Rectanglei::FromPositionSize({left, top}, {right - left, bottom - top});
}

[[nodiscard]] inline std::optional<FbRect>
filter_window_bounds(const BgfxLayerCompositeContext& ctx)
{
    if (ctx.scissor_state.enabled) {
        const Rml::Rectanglei scissor =
            clamp_scissor_to_surface(ctx.scissor_state.region, ctx.surface);
        if (scissor.Width() <= 0 || scissor.Height() <= 0) {
            return std::nullopt;
        }
        return FbRect{scissor.Left(), scissor.Top(), scissor.Width(), scissor.Height()};
    }
    return FbRect{0, 0, ctx.surface.framebuffer_width, ctx.surface.framebuffer_height};
}

[[nodiscard]] inline TextureRegion subregion(TextureRegion region, FbRect global_bounds)
{
    return texture_subregion_for_global_rect(region, global_bounds);
}

} // namespace rmlui_bgfx
