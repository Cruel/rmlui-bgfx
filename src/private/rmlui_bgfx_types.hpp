#pragma once

#include "rmlui_bgfx_bounds.hpp"
#include "rmlui_bgfx_pass_scheduler.hpp"
#include "rmlui_bgfx_planning.hpp"

#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/Types.h>
#include <bgfx/bgfx.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rmlui_bgfx {

using GlobalFbRect = FbRect;
using LocalFbRect = FbRect;

struct GeometryRecord {
    bgfx::VertexBufferHandle vb = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle ib = BGFX_INVALID_HANDLE;
    uint32_t index_count = 0;
    LogicalRect local_bounds;
};

struct TextureRecord {
    bgfx::TextureHandle handle = BGFX_INVALID_HANDLE;
    Rml::Vector2i dimensions;
    RenderBounds bounds;
    TextureOwnership ownership = TextureOwnership::External;
};

enum class ShaderRecordKind {
    Invalid,
    Gradient,
    Material,
};

struct ShaderRecord {
    ShaderRecordKind kind = ShaderRecordKind::Invalid;
    GradientRecord gradient;
    RmlUiMaterialShaderHandle material;
    Rml::Vector2f paint_dimensions;
    std::string value;
};

struct ScissorState {
    bool enabled = false;
    // Unless a call site explicitly names this as target-local, this rectangle is in global
    // framebuffer coordinates. Geometry, gradient, material, and clip-mask submissions convert it
    // to target-local space at the final draw boundary.
    Rml::Rectanglei region = Rml::Rectanglei::FromPositionSize({0, 0}, {0, 0});
};

enum class LayerKind {
    Root,
    VirtualChild,
};

enum class RecordedCommandKind {
    Geometry,
    Shader,
    ClipMask,
};

struct RecordedDrawCommand {
    RecordedCommandKind kind = RecordedCommandKind::Geometry;
    Rml::CompiledGeometryHandle geometry = 0;
    Rml::TextureHandle texture = 0;
    Rml::CompiledShaderHandle shader = 0;
    Rml::Vector2f translation;
    ScissorState scissor;
    bool transform_valid = false;
    std::array<float, 16> transform{};
    bool clip_mask_enabled = false;
    uint8_t stencil_ref = 1;
    Rml::ClipMaskOperation clip_operation = Rml::ClipMaskOperation::Set;
    uint8_t previous_ref = 1;
    uint8_t next_ref = 1;
};

struct TargetDescriptor {
    TargetRole role = TargetRole::LayerColorDepth;
    PostprocessTargetKind postprocess_kind = PostprocessTargetKind::Primary;
    TargetLifetime lifetime = TargetLifetime::Frame;
    GlobalFbRect bounds;
    int texture_width = 0;
    int texture_height = 0;
    bgfx::TextureFormat::Enum color_format = bgfx::TextureFormat::RGBA8;
    bgfx::TextureFormat::Enum depth_stencil_format = bgfx::TextureFormat::Unknown;
    uint8_t msaa_samples = 0;
    bool needs_depth_stencil = false;
    bool sampleable = true;
    bool blit_destination = false;
    uint64_t generation = 0;
    const char* debug_label = "RmlUi.Target";
    const char* reason = nullptr;
};

struct LayerRecord {
    bgfx::FrameBufferHandle framebuffer = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle color = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle depth_stencil = BGFX_INVALID_HANDLE;
    TargetLifetime target_lifetime = TargetLifetime::Viewport;
    uint64_t target_generation = 0;
    bgfx::TextureFormat::Enum color_format = bgfx::TextureFormat::RGBA8;
    bgfx::TextureFormat::Enum depth_stencil_format = bgfx::TextureFormat::Unknown;
    uint8_t msaa_samples = 0;
    RenderBounds bounds;
    GlobalFbRect valid_content_bounds;
    bool has_valid_content_bounds = false;
    ConservativeMaskBounds conservative_mask_bounds;
    // GL3 keeps layer targets viewport-sized, so transformed child content never needs to be
    // rebased into a bounded target. Until bounded transformed replay is fully validated, this
    // flag forces conservative/full-frame materialization for transformed recorded content.
    bool content_bounds_transform_fallback = false;
    // Inverse clip masks can make "valid content" be everything outside the geometry. Keep the
    // conservative container explicit instead of shrinking work to the inverse geometry bounds.
    bool content_bounds_inverse_mask_fallback = false;
    int texture_width = 0;
    int texture_height = 0;
    bool msaa_enabled = false;
    bool clip_mask_enabled = false;
    uint8_t stencil_ref = 1;
    std::vector<size_t> clip_commands;
    size_t inherited_clip_command_count = 0;
    float projection[16]{};

    LayerKind kind = LayerKind::Root;
    Rml::LayerHandle parent_layer = 0;
    ScissorState push_scissor;
    bool push_transform_valid = false;
    bool recording = false;
    bool materialized = false;
    bool clear_pending = false;
    std::vector<RecordedDrawCommand> commands;
};

// These helpers define the optimized path's per-materialization mapping contract. The input
// rectangles are global framebuffer coordinates unless named Local*, and local output is relative
// to the materialized layer target origin, not the viewport origin.
[[nodiscard]] inline Rml::Rectanglei clamp_scissor_local(Rml::Rectanglei global_scissor,
                                                         const GlobalFbRect& layer_fb_bounds)
{
    const int left =
        std::clamp(global_scissor.Left() - layer_fb_bounds.x, 0, std::max(layer_fb_bounds.w, 0));
    const int top =
        std::clamp(global_scissor.Top() - layer_fb_bounds.y, 0, std::max(layer_fb_bounds.h, 0));
    const int right =
        std::clamp(global_scissor.Right() - layer_fb_bounds.x, 0, std::max(layer_fb_bounds.w, 0));
    const int bottom =
        std::clamp(global_scissor.Bottom() - layer_fb_bounds.y, 0, std::max(layer_fb_bounds.h, 0));
    if (right <= left || bottom <= top) {
        return Rml::Rectanglei::FromPositionSize({0, 0}, {0, 0});
    }
    return Rml::Rectanglei::FromPositionSize({left, top}, {right - left, bottom - top});
}

[[nodiscard]] inline ScissorState scissor_local_to_layer(ScissorState scissor,
                                                         const RenderBounds& layer_bounds)
{
    if (!scissor.enabled)
        return scissor;
    return ScissorState{true, clamp_scissor_local(scissor.region, layer_bounds.framebuffer)};
}

[[nodiscard]] inline LocalFbRect active_stencil_clear_bounds(const LayerRecord& layer,
                                                             const ScissorState& scissor)
{
    if (!scissor.enabled) {
        return {0, 0, layer.texture_width, layer.texture_height};
    }
    const Rml::Rectanglei local_scissor =
        clamp_scissor_local(scissor.region, layer.bounds.framebuffer);
    return {local_scissor.Left(), local_scissor.Top(), local_scissor.Width(),
            local_scissor.Height()};
}

[[nodiscard]] inline LocalFbRect local_rect_for_layer(GlobalFbRect global_rect,
                                                      const LayerRecord& layer)
{
    const GlobalFbRect clipped = intersect(global_rect, layer.bounds.framebuffer);
    if (is_empty(clipped))
        return {};
    return {clipped.x - layer.bounds.framebuffer.x, clipped.y - layer.bounds.framebuffer.y,
            clipped.w, clipped.h};
}

[[nodiscard]] inline GlobalFbRect global_rect_for_layer(LocalFbRect local_rect,
                                                        const LayerRecord& layer)
{
    if (is_empty(local_rect))
        return {};
    return {local_rect.x + layer.bounds.framebuffer.x, local_rect.y + layer.bounds.framebuffer.y,
            local_rect.w, local_rect.h};
}

[[nodiscard]] inline LocalFbRect full_local_rect(const LayerRecord& layer)
{
    return {0, 0, layer.texture_width, layer.texture_height};
}

[[nodiscard]] inline Rml::Rectanglei rectangle_from_fb(FbRect rect)
{
    return Rml::Rectanglei::FromPositionSize({rect.x, rect.y}, {rect.w, rect.h});
}

struct SavedMaskRecord {
    Rml::CompiledFilterHandle filter = 0;
    PostprocessTargetKind target_kind = PostprocessTargetKind::BlendMask;
    bgfx::FrameBufferHandle framebuffer = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle color = BGFX_INVALID_HANDLE;
    uint64_t target_generation = 0;
    GlobalFbRect global_bounds;
    LocalFbRect local_rect;
    int texture_width = 0;
    int texture_height = 0;
    uint64_t source_layer_generation = 0;
    bool full_frame = false;
    bool bounded = false;
};

struct RenderTargetRecord {
    // Semantic postprocess role. Reusing a physical target must preserve this role identity.
    bgfx::FrameBufferHandle framebuffer = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle color = BGFX_INVALID_HANDLE;
    GlobalFbRect bounds;
    int texture_width = 0;
    int texture_height = 0;
    PostprocessTargetKind kind = PostprocessTargetKind::Primary;
    TargetLifetime lifetime = TargetLifetime::Frame;
    uint64_t generation = 0;
    bgfx::TextureFormat::Enum color_format = bgfx::TextureFormat::RGBA8;
    uint8_t msaa_samples = 0;
    uint64_t first_used_frame = 0;
    uint64_t last_used_frame = 0;
    bool full_frame = false;
    int surface_width = 0;
    int surface_height = 0;
};

struct TextureRegion {
    bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
    // Surface-space rectangle represented by local_rect inside this texture. This lets a compact
    // texture preserve GL3's global layer semantics without assuming the sampled pixels begin at
    // local origin {0, 0}.
    GlobalFbRect global_bounds;
    LocalFbRect local_rect;
    int texture_width = 0;
    int texture_height = 0;
};

struct CompositeFilterState {
    bool enabled = false;
    float opacity = 1.0f;
    std::array<float, 16> color_matrix{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

struct CompositeOp {
    TextureRegion source;
    bgfx::FrameBufferHandle destination = BGFX_INVALID_HANDLE;
    // Destination-target-local rectangle. Callers must convert from global output bounds with the
    // destination layer mapping before submitting a composite.
    LocalFbRect destination_rect;

    Rml::BlendMode blend_mode = Rml::BlendMode::Blend;
    // Destination-target-local scissor when enabled. Unlike generic ScissorState values, this one
    // is already converted before BgfxDrawContext::submit_composite() sees it.
    ScissorState scissor;
    bool apply_destination_stencil = false;
    bool msaa_enabled = false;
    uint8_t stencil_ref = 1;
    RmlUiPassKind kind = RmlUiPassKind::LayerComposite;
    RmlUiPassReason reason = RmlUiPassReason::LayerComposite;
    const char* name = "RmlUi.Composite";
    CompositeFilterState filter;
};

struct FilterApplyResult {
    TextureRegion output;
    CompositeFilterState composite_filter;
    // Conservative output bounds used for the externally composited filter result. These may
    // include transparent initialized padding required by blur/drop-shadow sampling.
    RenderBounds output_bounds;
    // Tighter semantic bounds of content that can contribute non-transparent pixels. This is
    // tracked separately so later passes can reason about valid content without changing the
    // conservative composited output region.
    RenderBounds valid_output_bounds;
};

struct ClipCommand {
    Rml::ClipMaskOperation operation = Rml::ClipMaskOperation::Set;
    Rml::CompiledGeometryHandle geometry = 0;
    Rml::Vector2f translation;
    ScissorState scissor;
    bool transform_valid = false;
    std::array<float, 16> transform{};
    uint8_t previous_ref = 1;
    uint8_t next_ref = 1;
};

struct PerfCounters {
#ifdef RMLUI_BGFX_ENABLE_RENDER_PERF
    uint32_t pass_count = 0;
    uint32_t view_count = 0;
    uint32_t view_reuses = 0;
    uint32_t base_clear_passes = 0;
    uint32_t layer_clear_passes = 0;
    uint32_t stencil_clear_passes = 0;
    uint32_t ordinary_geometry_passes = 0;
    uint32_t gradient_passes = 0;
    uint32_t clip_mask_passes = 0;
    uint32_t stencil_normalize_passes = 0;
    uint32_t filter_copy_passes = 0;
    uint32_t filter_opacity_passes = 0;
    uint32_t filter_color_matrix_passes = 0;
    uint32_t filter_mask_image_passes = 0;
    uint32_t filter_blur_passes = 0;
    uint32_t filter_drop_shadow_passes = 0;
    uint32_t filter_drop_shadow_composite_passes = 0;
    uint32_t color_filter_composite_folds = 0;
    uint32_t layer_scratch_copy_passes = 0;
    uint32_t layer_composite_reason_passes = 0;
    uint32_t final_composite_passes = 0;
    uint32_t save_texture_copy_passes = 0;
    uint32_t save_mask_copy_passes = 0;
    uint32_t other_copy_passes = 0;
    uint32_t other_passes = 0;
    uint32_t geometry_draws = 0;
    uint32_t geometry_indices = 0;
    uint32_t clip_mask_draws = 0;
    uint32_t gradient_draws = 0;
    uint32_t clear_passes = 0;
    uint32_t copy_passes = 0;
    uint32_t composite_passes = 0;
    uint32_t postprocess_passes = 0;
    uint32_t blur_passes = 0;
    uint32_t dropshadow_passes = 0;
    uint32_t mask_passes = 0;
    uint32_t layer_pushes = 0;
    uint32_t layer_clears = 0;
    uint32_t full_frame_layers = 0;
    uint32_t bounded_layers = 0;
    uint32_t full_frame_child_layers = 0;
    uint32_t bounded_child_layers = 0;
    uint32_t unbounded_layer_fallbacks = 0;
    uint32_t unbounded_no_scissor_fallbacks = 0;
    uint32_t unbounded_transform_fallbacks = 0;
    uint32_t unbounded_inverse_clip_fallbacks = 0;
    uint32_t full_frame_passes = 0;
    uint32_t bounded_passes = 0;
    uint32_t full_frame_clear_passes = 0;
    uint32_t bounded_clear_passes = 0;
    uint32_t full_frame_composite_passes = 0;
    uint32_t bounded_composite_passes = 0;
    uint32_t full_frame_postprocess_passes = 0;
    uint32_t bounded_postprocess_passes = 0;
    uint32_t full_frame_postprocess_targets = 0;
    uint32_t bounded_postprocess_targets = 0;
    uint32_t full_frame_postprocess_target_uses = 0;
    uint32_t bounded_postprocess_target_uses = 0;
    uint32_t direct_base_presentations = 0;
    uint32_t offscreen_base_presentations = 0;
    uint32_t direct_base_fallbacks = 0;
    uint32_t layer_allocations = 0;
    uint32_t layer_destroys = 0;
    uint32_t postprocess_allocations = 0;
    uint32_t postprocess_destroys = 0;
    uint64_t geometry_estimated_pixels = 0;
    uint64_t clip_estimated_pixels = 0;
    uint64_t clear_pixels = 0;
    uint64_t copy_pixels = 0;
    uint64_t composite_pixels = 0;
    uint64_t postprocess_pixels = 0;
    uint32_t max_layer_width = 0;
    uint32_t max_layer_height = 0;
    uint32_t max_child_layer_width = 0;
    uint32_t max_child_layer_height = 0;
    uint32_t max_postprocess_width = 0;
    uint32_t max_postprocess_height = 0;

    void reset() { *this = PerfCounters{}; }

    void add_pass(bool reused, RmlUiPassReason reason)
    {
        pass_count++;
        if (reused) {
            view_reuses++;
        } else {
            view_count++;
        }
        switch (reason) {
        case RmlUiPassReason::OrdinaryGeometry:
            ordinary_geometry_passes++;
            break;
        case RmlUiPassReason::Gradient:
            gradient_passes++;
            break;
        case RmlUiPassReason::ClipMask:
            clip_mask_passes++;
            break;
        case RmlUiPassReason::StencilNormalize:
            stencil_normalize_passes++;
            break;
        case RmlUiPassReason::BaseClear:
            base_clear_passes++;
            break;
        case RmlUiPassReason::LayerClear:
            layer_clear_passes++;
            break;
        case RmlUiPassReason::StencilClear:
            stencil_clear_passes++;
            break;
        case RmlUiPassReason::FilterCopy:
            filter_copy_passes++;
            break;
        case RmlUiPassReason::FilterOpacity:
            filter_opacity_passes++;
            break;
        case RmlUiPassReason::FilterColorMatrix:
            filter_color_matrix_passes++;
            break;
        case RmlUiPassReason::FilterMaskImage:
            filter_mask_image_passes++;
            break;
        case RmlUiPassReason::FilterBlur:
            filter_blur_passes++;
            break;
        case RmlUiPassReason::FilterDropShadow:
            filter_drop_shadow_passes++;
            break;
        case RmlUiPassReason::FilterDropShadowComposite:
            filter_drop_shadow_composite_passes++;
            break;
        case RmlUiPassReason::LayerScratchCopy:
            layer_scratch_copy_passes++;
            break;
        case RmlUiPassReason::LayerComposite:
            layer_composite_reason_passes++;
            break;
        case RmlUiPassReason::FinalComposite:
            final_composite_passes++;
            break;
        case RmlUiPassReason::SaveTextureCopy:
            save_texture_copy_passes++;
            break;
        case RmlUiPassReason::SaveMaskCopy:
            save_mask_copy_passes++;
            break;
        case RmlUiPassReason::OtherCopy:
            other_copy_passes++;
            break;
        case RmlUiPassReason::Other:
            other_passes++;
            break;
        }
    }
    void add_color_filter_composite_fold() { color_filter_composite_folds++; }
    void add_geometry(uint64_t px, uint32_t indices)
    {
        geometry_draws++;
        geometry_indices += indices;
        geometry_estimated_pixels += px;
    }
    void add_clip_mask(uint64_t px)
    {
        clip_mask_draws++;
        clip_estimated_pixels += px;
    }
    void add_gradient() { gradient_draws++; }
    void add_clear(uint64_t px, bool full_frame)
    {
        clear_passes++;
        clear_pixels += px;
        if (full_frame) {
            full_frame_passes++;
            full_frame_clear_passes++;
        } else {
            bounded_passes++;
            bounded_clear_passes++;
        }
    }
    void add_copy() { copy_passes++; }
    void add_copy_pixels(uint64_t px) { copy_pixels += px; }
    void add_composite(uint64_t px, bool full_frame)
    {
        composite_passes++;
        composite_pixels += px;
        if (full_frame) {
            full_frame_passes++;
            full_frame_composite_passes++;
        } else {
            bounded_passes++;
            bounded_composite_passes++;
        }
    }
    void add_postprocess(uint64_t px, bool full_frame)
    {
        postprocess_passes++;
        postprocess_pixels += px;
        if (full_frame) {
            full_frame_passes++;
            full_frame_postprocess_passes++;
        } else {
            bounded_passes++;
            bounded_postprocess_passes++;
        }
    }
    void add_blur() { blur_passes++; }
    void add_dropshadow() { dropshadow_passes++; }
    void add_mask(uint64_t px, bool full_frame)
    {
        mask_passes++;
        postprocess_pixels += px;
        if (full_frame) {
            full_frame_passes++;
            full_frame_postprocess_passes++;
        } else {
            bounded_passes++;
            bounded_postprocess_passes++;
        }
    }
    void add_layer_push() { layer_pushes++; }
    void add_layer_clear() { layer_clears++; }
    void add_full_frame_layer() { full_frame_layers++; }
    void add_bounded_layer() { bounded_layers++; }
    void add_full_frame_child_layer()
    {
        full_frame_layers++;
        full_frame_child_layers++;
    }
    void add_bounded_child_layer()
    {
        bounded_layers++;
        bounded_child_layers++;
    }
    void add_unbounded_layer_fallback(bool no_scissor, bool transform, bool inverse_clip = false)
    {
        unbounded_layer_fallbacks++;
        if (no_scissor)
            unbounded_no_scissor_fallbacks++;
        if (transform)
            unbounded_transform_fallbacks++;
        if (inverse_clip)
            unbounded_inverse_clip_fallbacks++;
    }
    void add_full_frame_pp_target() { full_frame_postprocess_targets++; }
    void add_bounded_pp_target() { bounded_postprocess_targets++; }
    void add_postprocess_target_use(uint32_t w, uint32_t h, bool full_frame)
    {
        update_pp_max(w, h);
        if (full_frame) {
            full_frame_postprocess_target_uses++;
        } else {
            bounded_postprocess_target_uses++;
        }
    }
    void add_direct_base_presentation() { direct_base_presentations++; }
    void add_offscreen_base_presentation() { offscreen_base_presentations++; }
    void add_direct_base_fallback() { direct_base_fallbacks++; }
    void add_layer_alloc(uint32_t, uint32_t) { layer_allocations++; }
    void update_layer_max(uint32_t w, uint32_t h)
    {
        max_layer_width = std::max(max_layer_width, w);
        max_layer_height = std::max(max_layer_height, h);
    }
    void update_child_layer_max(uint32_t w, uint32_t h)
    {
        max_child_layer_width = std::max(max_child_layer_width, w);
        max_child_layer_height = std::max(max_child_layer_height, h);
    }
    void add_layer_destroy() { layer_destroys++; }
    void add_pp_alloc(uint32_t, uint32_t) { postprocess_allocations++; }
    void update_pp_max(uint32_t w, uint32_t h)
    {
        max_postprocess_width = std::max(max_postprocess_width, w);
        max_postprocess_height = std::max(max_postprocess_height, h);
    }
    void add_pp_destroy() { postprocess_destroys++; }
#else
    void reset() {}
    void add_pass(bool, RmlUiPassReason) {}
    void add_color_filter_composite_fold() {}
    void add_geometry(uint64_t, uint32_t) {}
    void add_clip_mask(uint64_t) {}
    void add_gradient() {}
    void add_clear(uint64_t, bool) {}
    void add_copy() {}
    void add_copy_pixels(uint64_t) {}
    void add_composite(uint64_t, bool) {}
    void add_postprocess(uint64_t, bool) {}
    void add_blur() {}
    void add_dropshadow() {}
    void add_mask(uint64_t, bool) {}
    void add_layer_push() {}
    void add_layer_clear() {}
    void add_full_frame_layer() {}
    void add_bounded_layer() {}
    void add_full_frame_child_layer() {}
    void add_bounded_child_layer() {}
    void add_unbounded_layer_fallback(bool, bool, bool = false) {}
    void add_full_frame_pp_target() {}
    void add_bounded_pp_target() {}
    void add_postprocess_target_use(uint32_t, uint32_t, bool) {}
    void add_direct_base_presentation() {}
    void add_offscreen_base_presentation() {}
    void add_direct_base_fallback() {}
    void add_layer_alloc(uint32_t, uint32_t) {}
    void update_layer_max(uint32_t, uint32_t) {}
    void update_child_layer_max(uint32_t, uint32_t) {}
    void add_layer_destroy() {}
    void add_pp_alloc(uint32_t, uint32_t) {}
    void update_pp_max(uint32_t, uint32_t) {}
    void add_pp_destroy() {}
#endif
};

} // namespace rmlui_bgfx
