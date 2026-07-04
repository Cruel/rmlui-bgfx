#include "rmlui_bgfx_layers.hpp"
#include "rmlui_bgfx_layer_composite_helpers.hpp"
#include "rmlui_bgfx_layer_paths.hpp"
#include "rmlui_bgfx_trace.hpp"

#include <algorithm>

namespace rmlui_bgfx {

namespace {

[[nodiscard]] bool is_full_frame_surface(FbRect rect, const SurfaceMetrics& surface)
{
    return !is_empty(rect) && rect.x == 0 && rect.y == 0 && rect.w >= surface.framebuffer_width &&
           rect.h >= surface.framebuffer_height;
}

} // namespace

BgfxLayerSystem::BgfxLayerSystem(BgfxTargetCache& target_cache) : m_target_cache(&target_cache) {}

void BgfxLayerSystem::begin_frame()
{
    if (m_target_cache) {
        m_target_cache->begin_frame();
    }
    m_layer_stack.clear();
    m_layer_stack.push_back(0);
    m_active_layer = 0;
}

void BgfxLayerSystem::clear_stack_to_base()
{
    m_layer_stack.clear();
    m_active_layer = 0;
}

void BgfxLayerSystem::push_layer(Rml::LayerHandle handle)
{
    m_layer_stack.push_back(handle);
    m_active_layer = handle;
}

bool BgfxLayerSystem::pop_layer()
{
    if (m_layer_stack.size() <= 1) {
        return false;
    }
    m_layer_stack.pop_back();
    m_active_layer = m_layer_stack.back();
    return true;
}

LayerRecord& BgfxLayerSystem::prepare_virtual_child(Rml::LayerHandle handle,
                                                    Rml::LayerHandle parent,
                                                    const RenderBounds& provisional_bounds,
                                                    ScissorState push_scissor,
                                                    bool push_transform_valid)
{
    LayerRecord& previous = m_target_cache->prepare_virtual_layer_slot(uint32_t(handle));
    LayerRecord preserved_resources;
    preserved_resources.framebuffer = previous.framebuffer;
    preserved_resources.color = previous.color;
    preserved_resources.depth_stencil = previous.depth_stencil;
    preserved_resources.target_lifetime = previous.target_lifetime;
    preserved_resources.target_generation = previous.target_generation;
    preserved_resources.color_format = previous.color_format;
    preserved_resources.depth_stencil_format = previous.depth_stencil_format;
    preserved_resources.msaa_samples = previous.msaa_samples;
    preserved_resources.texture_width = previous.texture_width;
    preserved_resources.texture_height = previous.texture_height;
    preserved_resources.msaa_enabled = previous.msaa_enabled;
    previous.framebuffer = BGFX_INVALID_HANDLE;
    previous.color = BGFX_INVALID_HANDLE;
    previous.depth_stencil = BGFX_INVALID_HANDLE;
    previous.target_generation = 0;
    previous.texture_width = 0;
    previous.texture_height = 0;

    LayerRecord child;
    child.framebuffer = preserved_resources.framebuffer;
    child.color = preserved_resources.color;
    child.depth_stencil = preserved_resources.depth_stencil;
    child.target_lifetime = preserved_resources.target_lifetime;
    child.target_generation = preserved_resources.target_generation;
    child.color_format = preserved_resources.color_format;
    child.depth_stencil_format = preserved_resources.depth_stencil_format;
    child.msaa_samples = preserved_resources.msaa_samples;
    child.texture_width = preserved_resources.texture_width;
    child.texture_height = preserved_resources.texture_height;
    child.msaa_enabled = preserved_resources.msaa_enabled;
    child.kind = LayerKind::VirtualChild;
    child.parent_layer = parent;
    child.bounds = provisional_bounds;
    child.push_scissor = push_scissor;
    child.push_transform_valid = push_transform_valid;
    child.recording = true;
    child.materialized = false;
    child.clear_pending = true;

    if (const LayerRecord* parent_layer = layer_for_handle(parent)) {
        child.clip_mask_enabled = parent_layer->clip_mask_enabled;
        child.stencil_ref =
            parent_layer->clip_mask_enabled ? parent_layer->stencil_ref : uint8_t(1);
        if (parent_layer->clip_mask_enabled) {
            child.conservative_mask_bounds = parent_layer->conservative_mask_bounds;
            child.clip_commands = parent_layer->clip_commands;
            child.inherited_clip_command_count = child.clip_commands.size();
        }
    }

    previous = std::move(child);
    return previous;
}

LayerRecord* BgfxLayerSystem::layer_for_handle(Rml::LayerHandle handle)
{
    if (!m_target_cache) {
        return nullptr;
    }
    return m_target_cache->layer(uint32_t(handle));
}

const LayerRecord* BgfxLayerSystem::layer_for_handle(Rml::LayerHandle handle) const
{
    if (!m_target_cache) {
        return nullptr;
    }
    return m_target_cache->layer(uint32_t(handle));
}

LayerRecord* BgfxLayerSystem::materialized_layer_for_handle(Rml::LayerHandle handle,
                                                            bool direct_base_requested)
{
    LayerRecord* layer = layer_for_handle(handle);
    if (!layer) {
        return nullptr;
    }
    if (size_t(handle) == 0 && direct_base_requested) {
        return layer;
    }
    if (!bgfx::isValid(layer->framebuffer)) {
        return nullptr;
    }
    return layer;
}

const LayerRecord* BgfxLayerSystem::materialized_layer_for_handle(Rml::LayerHandle handle,
                                                                  bool direct_base_requested) const
{
    const LayerRecord* layer = layer_for_handle(handle);
    if (!layer) {
        return nullptr;
    }
    if (size_t(handle) == 0 && direct_base_requested) {
        return layer;
    }
    if (!bgfx::isValid(layer->framebuffer)) {
        return nullptr;
    }
    return layer;
}

LayerRecord* BgfxLayerSystem::current_layer() { return layer_for_handle(m_active_layer); }

const LayerRecord* BgfxLayerSystem::current_layer() const
{
    return layer_for_handle(m_active_layer);
}

bool BgfxLayerSystem::active_layer_is_recording() const
{
    const LayerRecord* layer = layer_for_handle(m_active_layer);
    if (!layer) {
        return false;
    }
    return layer->kind == LayerKind::VirtualChild && layer->recording && !layer->materialized;
}

bool BgfxLayerSystem::materialize_layer(const BgfxLayerMaterializeContext& ctx,
                                        Rml::LayerHandle handle,
                                        std::optional<FbRect> required_bounds)
{
    LayerRecord* layer = layer_for_handle(handle);
    if (!layer) {
        RMLUI_BGFX_TRACE_FAILURE(ctx.trace, "MaterializeLayer", "invalid layer handle", {
            line.field("layer", size_t(handle));
        });
        return false;
    }
    if (layer->kind == LayerKind::Root) {
        RMLUI_BGFX_TRACE(ctx.trace, TraceCategory::Layer, "skip", "MaterializeLayer", {
            line.field("layer", size_t(handle));
            line.field("reason", "root layer");
        });
        return true;
    }
    if (!ctx.choose_bounds || !ctx.ensure_layer || !ctx.clear_layer || !ctx.replay_clip_commands ||
        !ctx.replay_recorded_commands) {
        RMLUI_BGFX_TRACE_FAILURE(ctx.trace, "MaterializeLayer", "missing callbacks", {
            line.field("layer", size_t(handle));
        });
        return false;
    }
    if (layer->materialized) {
        const FbRect current_overlap =
            required_bounds ? intersect(layer->bounds.framebuffer, *required_bounds) : FbRect{};
        const bool required_contained =
            required_bounds && current_overlap.x == required_bounds->x &&
            current_overlap.y == required_bounds->y && current_overlap.w == required_bounds->w &&
            current_overlap.h == required_bounds->h;
        const bool transparent_margin_only =
            required_bounds && !is_empty(current_overlap) && layer->commands.empty() &&
            layer->has_valid_content_bounds &&
            !is_empty(intersect(layer->valid_content_bounds, layer->bounds.framebuffer));
        if (!required_bounds || is_empty(*required_bounds) || required_contained ||
            transparent_margin_only) {
            RMLUI_BGFX_TRACE(ctx.trace, TraceCategory::Layer, "reuse", "MaterializeLayer", {
                line.field("layer", size_t(handle));
                line.fb_rect("bounds", layer->bounds.framebuffer);
                line.fb_rect("required", required_bounds ? *required_bounds : FbRect{});
                line.field("command_count", layer->commands.size());
                line.field("transparent_margin", transparent_margin_only);
            });
            return true;
        }
        RMLUI_BGFX_TRACE(ctx.trace, TraceCategory::Layer, "plan", "MaterializeLayerResize", {
            line.field("layer", size_t(handle));
            line.fb_rect("old_bounds", layer->bounds.framebuffer);
            line.fb_rect("required", *required_bounds);
        });
        layer->materialized = false;
        layer->clear_pending = true;
    }

    RenderBounds child_bounds = ctx.choose_bounds(*layer, required_bounds);
    const bool bounded = !is_full_frame_surface(child_bounds.framebuffer, ctx.surface);
    RMLUI_BGFX_TRACE(ctx.trace, TraceCategory::Layer, "materialize", "MaterializeLayer", {
        line.field("layer", size_t(handle));
        line.field("kind", int(layer->kind));
        line.field("recording", layer->recording);
        line.field("command_count", layer->commands.size());
        line.fb_rect("required", required_bounds ? *required_bounds : FbRect{});
        line.fb_rect("chosen", child_bounds.framebuffer);
        line.logical_rect("chosen_logical", child_bounds.logical);
        line.field("bounded", bounded);
        line.field("push_transform", layer->push_transform_valid);
        line.scissor("push_scissor", layer->push_scissor);
    });
    if (!ctx.ensure_layer(size_t(handle), child_bounds)) {
        RMLUI_BGFX_TRACE_FAILURE(ctx.trace, "MaterializeLayer", "ensure layer target failed", {
            line.field("layer", size_t(handle));
            line.fb_rect("chosen", child_bounds.framebuffer);
        });
        return false;
    }
    layer = layer_for_handle(handle);
    if (!layer) {
        RMLUI_BGFX_TRACE_FAILURE(ctx.trace, "MaterializeLayer", "layer missing after ensure", {
            line.field("layer", size_t(handle));
        });
        return false;
    }
    layer->recording = false;
    layer->materialized = true;
    const bool final_clip_mask_enabled = layer->clip_mask_enabled;
    const uint8_t final_stencil_ref = layer->stencil_ref;

    if (layer->clear_pending) {
        if (!ctx.clear_layer(handle, bounded)) {
            RMLUI_BGFX_TRACE_FAILURE(ctx.trace, "MaterializeLayer", "clear failed", {
                line.field("layer", size_t(handle));
                line.fb_rect("bounds", layer->bounds.framebuffer);
            });
            return false;
        }
        layer = layer_for_handle(handle);
        if (!layer) {
            return false;
        }
        layer->clear_pending = false;
        RMLUI_BGFX_TRACE(ctx.trace, TraceCategory::Layer, "clear", "MaterializeLayer", {
            line.field("layer", size_t(handle));
            line.fb_rect("bounds", layer->bounds.framebuffer);
            line.field("bounded", bounded);
        });
    }

    if (layer->inherited_clip_command_count > 0) {
        const size_t count =
            std::min(layer->inherited_clip_command_count, layer->clip_commands.size());
        const std::vector<size_t> inherited_commands(layer->clip_commands.begin(),
                                                     layer->clip_commands.begin() + count);
        ctx.replay_clip_commands(handle, inherited_commands);
        RMLUI_BGFX_TRACE(ctx.trace, TraceCategory::Clip, "replay",
                         "MaterializeLayerInheritedClips", {
                             line.field("layer", size_t(handle));
                             line.field("count", inherited_commands.size());
                         });
        layer = layer_for_handle(handle);
        if (!layer) {
            RMLUI_BGFX_TRACE_FAILURE(ctx.trace, "MaterializeLayer", "layer missing after clips", {
                line.field("layer", size_t(handle));
            });
            return false;
        }
        layer->clip_mask_enabled = final_clip_mask_enabled;
        layer->stencil_ref = final_stencil_ref;
    }
    const bool replayed = ctx.replay_recorded_commands(handle);
    RMLUI_BGFX_TRACE(ctx.trace, replayed ? TraceCategory::Layer : TraceCategory::Failure,
                     replayed ? "replay" : "fail", "MaterializeLayerReplay", {
                         line.field("layer", size_t(handle));
                         line.field("ok", replayed);
                     });
    return replayed;
}

void BgfxLayerSystem::composite_layers(const BgfxLayerCompositeContext& ctx,
                                       Rml::LayerHandle source, Rml::LayerHandle destination,
                                       Rml::BlendMode blend_mode,
                                       Rml::Span<const Rml::CompiledFilterHandle> filters)
{
    switch (ctx.render_path) {
    case RenderPath::Reference:
        return;
    case RenderPath::Optimized:
        composite_layers_optimized(*this, ctx, source, destination, blend_mode, filters);
        return;
    }
}

Rml::TextureHandle BgfxLayerSystem::save_layer_as_texture(const BgfxLayerSaveTextureContext& ctx)
{
    // GL3 defines this operation by the current scissor rectangle, not by content bounds. Preserve
    // the requested output size and pad/copy overlap explicitly when optimized materialization is
    // tighter than the callback texture bounds.
    if (ctx.direct_base_requested && size_t(m_active_layer) == 0) {
        RMLUI_BGFX_TRACE(ctx.trace, TraceCategory::Texture, "skip", "SaveLayerAsTexture", {
            line.field("reason", "direct base preservation required");
        });
        if (ctx.root_requires_preservation) {
            *ctx.root_requires_preservation = true;
        }
        if (ctx.fail_frame) {
            ctx.fail_frame("SaveLayerAsTexture requires offscreen root");
        }
        return 0;
    }
    if (!ctx.current_save_bounds) {
        RMLUI_BGFX_TRACE_FAILURE(ctx.trace, "SaveLayerAsTexture", "missing current_save_bounds", {
            line.field("active_layer", size_t(m_active_layer));
        });
        return 0;
    }
    const Rml::Rectanglei bounds = ctx.current_save_bounds();
    if (bounds.Width() <= 0 || bounds.Height() <= 0) {
        RMLUI_BGFX_TRACE_FAILURE(ctx.trace, "SaveLayerAsTexture", "empty save bounds", {
            line.rml_rect("save_bounds", bounds);
        });
        return 0;
    }
    const FbRect global_bounds{bounds.Left(), bounds.Top(), bounds.Width(), bounds.Height()};

    if (!ctx.materialize_layer ||
        !ctx.materialize_layer(m_active_layer, std::optional<FbRect>{global_bounds})) {
        if (ctx.fail_frame) {
            ctx.fail_frame("SaveLayerAsTexture failed to materialize layer");
        }
        RMLUI_BGFX_TRACE_FAILURE(ctx.trace, "SaveLayerAsTexture", "materialize failed", {
            line.field("layer", size_t(m_active_layer));
            line.fb_rect("global_bounds", global_bounds);
        });
        return 0;
    }
    LayerRecord* layer = materialized_layer_for_handle(m_active_layer, ctx.direct_base_requested);
    if (!layer || !bgfx::isValid(layer->color) || !ctx.copy_region_to_texture || !ctx.textures ||
        !ctx.texture_counter) {
        RMLUI_BGFX_TRACE_FAILURE(ctx.trace, "SaveLayerAsTexture", "invalid layer or callbacks", {
            line.field("layer", size_t(m_active_layer));
            line.field("has_layer", layer != nullptr);
            line.field("has_color", layer && bgfx::isValid(layer->color));
        });
        return 0;
    }
    // Convert the requested global framebuffer save rectangle into the compact layer target's
    // local sampling rectangle. The output texture still keeps the original global/scissor size.
    const FbRect local_bounds = local_rect_for_layer(global_bounds, *layer);
    if (is_empty(local_bounds)) {
        RMLUI_BGFX_TRACE_FAILURE(ctx.trace, "SaveLayerAsTexture", "empty local bounds", {
            line.field("layer", size_t(m_active_layer));
            line.fb_rect("global_bounds", global_bounds);
            line.fb_rect("layer_bounds", layer->bounds.framebuffer);
        });
        return 0;
    }

    bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
    const Rml::Vector2i output_dimensions{bounds.Width(), bounds.Height()};
    Rml::Rectanglei copy_bounds = rectangle_from_fb(local_bounds);
    // Rebase the requested global save rect into the compact render target. This is not a visual
    // flip; saved-layer replay corrects callback texture coordinates before draw submission.
    if (layer->texture_height > local_bounds.h) {
        const int sample_top = layer->texture_height - copy_bounds.Bottom();
        copy_bounds = Rml::Rectanglei::FromPositionSize(
            {copy_bounds.Left(), sample_top}, {copy_bounds.Width(), copy_bounds.Height()});
    }
    if (local_bounds.w == global_bounds.w && local_bounds.h == global_bounds.h) {
        texture =
            ctx.copy_region_to_texture(layer->color, copy_bounds, layer->texture_width,
                                       layer->texture_height, "RmlUi.SaveLayerAsTexture", false);
    } else if (ctx.copy_region_to_sized_texture) {
        const FbRect overlap_global = intersect(global_bounds, layer->bounds.framebuffer);
        // Offset is in saved-texture-local coordinates, preserving GL3's requested scissor-sized
        // output even when the bounded layer only contains an overlapping subregion.
        const Rml::Vector2i destination_offset{overlap_global.x - global_bounds.x,
                                               overlap_global.y - global_bounds.y};
        texture = ctx.copy_region_to_sized_texture(
            layer->color, copy_bounds, layer->texture_width, layer->texture_height,
            output_dimensions, destination_offset, "RmlUi.SaveLayerAsTexture", false);
    }
    if (!bgfx::isValid(texture)) {
        if (ctx.fail_frame) {
            ctx.fail_frame("SaveLayerAsTexture failed to copy layer contents");
        }
        RMLUI_BGFX_TRACE_FAILURE(ctx.trace, "SaveLayerAsTexture", "copy failed", {
            line.field("layer", size_t(m_active_layer));
            line.rml_rect("copy_bounds", copy_bounds);
            line.field("output_w", output_dimensions.x);
            line.field("output_h", output_dimensions.y);
        });
        return 0;
    }

    const Rml::TextureHandle handle = ++(*ctx.texture_counter);
    ctx.textures->emplace(
        handle,
        TextureRecord{texture,
                      {bounds.Width(), bounds.Height()},
                      RenderBounds{{float(bounds.Left()), float(bounds.Top()),
                                    float(bounds.Width()), float(bounds.Height())},
                                   {bounds.Left(), bounds.Top(), bounds.Width(), bounds.Height()}},
                      TextureOwnership::SavedLayer});
    RMLUI_BGFX_TRACE(ctx.trace, TraceCategory::Texture, "copy", "SaveLayerAsTexture", {
        line.field("layer", size_t(m_active_layer));
        line.field("texture_handle", handle);
        line.handle("bgfx_tex", texture);
        line.rml_rect("save_bounds", bounds);
        line.fb_rect("global_bounds", global_bounds);
        line.fb_rect("local_bounds", local_bounds);
        line.rml_rect("copy_bounds", copy_bounds);
        line.field("output_w", output_dimensions.x);
        line.field("output_h", output_dimensions.y);
    });
    return handle;
}

Rml::CompiledFilterHandle
BgfxLayerSystem::save_layer_as_mask_image(const BgfxLayerSaveMaskContext& ctx)
{
    // GL3 saves into a renderer-owned blend-mask target and returns a MaskImage filter, not an
    // ordinary saved texture. Store explicit SavedMaskRecord ownership so later MaskImage
    // resolution uses this exact target/generation rather than anonymous BlendMask lookup.
    if (ctx.direct_base_requested && size_t(m_active_layer) == 0) {
        RMLUI_BGFX_TRACE(ctx.trace, TraceCategory::Mask, "skip", "SaveLayerAsMaskImage", {
            line.field("reason", "direct base preservation required");
        });
        if (ctx.root_requires_preservation) {
            *ctx.root_requires_preservation = true;
        }
        if (ctx.fail_frame) {
            ctx.fail_frame("SaveLayerAsMaskImage requires offscreen root");
        }
        return 0;
    }
    if (!ctx.saved_masks || !ctx.materialize_layer ||
        !ctx.materialize_layer(m_active_layer, std::nullopt)) {
        if (ctx.fail_frame) {
            ctx.fail_frame("SaveLayerAsMaskImage failed to materialize layer");
        }
        RMLUI_BGFX_TRACE_FAILURE(ctx.trace, "SaveLayerAsMaskImage", "materialize failed", {
            line.field("layer", size_t(m_active_layer));
        });
        return 0;
    }
    LayerRecord* layer = materialized_layer_for_handle(m_active_layer, ctx.direct_base_requested);
    if (!layer || !bgfx::isValid(layer->color) || !ctx.filters || !ctx.saved_masks ||
        !ctx.filter_counter || !ctx.ensure_target || !ctx.composite) {
        RMLUI_BGFX_TRACE_FAILURE(ctx.trace, "SaveLayerAsMaskImage",
                                 "invalid layer or callbacks", {
                                     line.field("layer", size_t(m_active_layer));
                                     line.field("has_layer", layer != nullptr);
                                     line.field("has_color", layer && bgfx::isValid(layer->color));
                                 });
        return 0;
    }
    TextureRegion source =
        make_layer_texture_region(layer->color, layer->bounds.framebuffer, full_local_rect(*layer),
                                  layer->texture_width, layer->texture_height);
    // Keep the saved mask in framebuffer coordinates, but allocate only the materialized source
    // layer's physical coverage. MaskImage sampling treats pixels outside this rectangle as
    // transparent through the UV bounds check in the mask multiply shader.
    FbRect mask_global_bounds =
        clamp_to_surface(align_outward_for_render_target(source.global_bounds), ctx.surface);
    if (is_empty(mask_global_bounds)) {
        RMLUI_BGFX_TRACE_FAILURE(ctx.trace, "SaveLayerAsMaskImage", "empty mask bounds", {
            line.fb_rect("mask_bounds", mask_global_bounds);
        });
        return 0;
    }

    RenderTargetRecord* blend_mask =
        ctx.ensure_target(PostprocessTargetKind::BlendMask, mask_global_bounds);
    if (!blend_mask || !bgfx::isValid(blend_mask->framebuffer) ||
        !bgfx::isValid(blend_mask->color)) {
        if (ctx.fail_frame) {
            ctx.fail_frame("SaveLayerAsMaskImage target allocation failed");
        }
        RMLUI_BGFX_TRACE_FAILURE(ctx.trace, "SaveLayerAsMaskImage", "target allocation failed", {
            line.fb_rect("mask_bounds", mask_global_bounds);
        });
        return 0;
    }

    source = subregion(source, mask_global_bounds);
    if (is_empty(source.global_bounds) || is_empty(source.local_rect)) {
        RMLUI_BGFX_TRACE_FAILURE(ctx.trace, "SaveLayerAsMaskImage", "empty source region", {
            line.texture_region("source", source);
            line.fb_rect("layer_bounds", layer->bounds.framebuffer);
            line.fb_rect("mask_bounds", mask_global_bounds);
        });
        return 0;
    }
    const LocalFbRect destination_rect{source.global_bounds.x - mask_global_bounds.x,
                                       source.global_bounds.y - mask_global_bounds.y,
                                       source.global_bounds.w, source.global_bounds.h};
    if (is_empty(destination_rect)) {
        RMLUI_BGFX_TRACE_FAILURE(ctx.trace, "SaveLayerAsMaskImage", "empty destination rect", {
            line.texture_region("source", source);
            line.fb_rect("mask_bounds", mask_global_bounds);
            line.local_rect("destination", destination_rect);
        });
        return 0;
    }

    if (!ctx.composite(make_layer_composite_op(
            source, blend_mask->framebuffer, Rml::BlendMode::Replace, ScissorState{false, {}},
            false, 1, RmlUiPassKind::Copy, RmlUiPassReason::FilterMaskImage,
            "RmlUi.SaveLayerAsMaskImage", destination_rect))) {
        if (ctx.fail_frame) {
            ctx.fail_frame("SaveLayerAsMaskImage failed to copy layer contents");
        }
        RMLUI_BGFX_TRACE_FAILURE(ctx.trace, "SaveLayerAsMaskImage", "copy failed", {
            line.texture_region("source", source);
            line.local_rect("destination", destination_rect);
        });
        return 0;
    }

    FilterRecord filter;
    filter.kind = FilterKind::MaskImage;
    filter.resource = 0;
    filter.mask_bounds = {mask_global_bounds.x, mask_global_bounds.y, mask_global_bounds.w,
                          mask_global_bounds.h};
    const Rml::CompiledFilterHandle handle = ++(*ctx.filter_counter);
    SavedMaskRecord saved_mask;
    saved_mask.filter = handle;
    saved_mask.target_kind = PostprocessTargetKind::BlendMask;
    saved_mask.framebuffer = blend_mask->framebuffer;
    saved_mask.color = blend_mask->color;
    saved_mask.target_generation = blend_mask->generation;
    saved_mask.global_bounds = mask_global_bounds;
    saved_mask.local_rect = {0, 0, blend_mask->texture_width, blend_mask->texture_height};
    saved_mask.texture_width = blend_mask->texture_width;
    saved_mask.texture_height = blend_mask->texture_height;
    saved_mask.source_layer_generation = layer->target_generation;
    saved_mask.full_frame = !is_empty(mask_global_bounds) && mask_global_bounds.x == 0 &&
                            mask_global_bounds.y == 0 &&
                            mask_global_bounds.w >= ctx.surface.framebuffer_width &&
                            mask_global_bounds.h >= ctx.surface.framebuffer_height;
    saved_mask.bounded = !saved_mask.full_frame;

    const auto filter_inserted = ctx.filters->emplace(handle, filter);
    const auto mask_inserted = ctx.saved_masks->emplace(handle, saved_mask);
    if (!filter_inserted.second || !mask_inserted.second) {
        ctx.filters->erase(handle);
        ctx.saved_masks->erase(handle);
        RMLUI_BGFX_TRACE_FAILURE(ctx.trace, "SaveLayerAsMaskImage", "registry insert failed", {
            line.field("filter", handle);
        });
        return 0;
    }
    RMLUI_BGFX_TRACE(ctx.trace, TraceCategory::Mask, "copy", "SaveLayerAsMaskImage", {
        line.field("layer", size_t(m_active_layer));
        line.field("filter", handle);
        line.texture_region("source", source);
        line.fb_rect("mask_bounds", mask_global_bounds);
        line.local_rect("destination", destination_rect);
        line.handle("mask_fb", blend_mask->framebuffer);
        line.handle("mask_tex", blend_mask->color);
        line.field("target_generation", blend_mask->generation);
        line.field("full_frame", saved_mask.full_frame);
    });
    return handle;
}

} // namespace rmlui_bgfx
