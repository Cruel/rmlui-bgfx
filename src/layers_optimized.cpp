#include "rmlui_bgfx_layer_paths.hpp"

#include "rmlui_bgfx_layer_composite_helpers.hpp"

namespace rmlui_bgfx {

void composite_layers_optimized(BgfxLayerSystem& layer_system,
                                const BgfxLayerCompositeContext& ctx,
                                Rml::LayerHandle source,
                                Rml::LayerHandle destination,
                                Rml::BlendMode blend_mode,
                                Rml::Span<const Rml::CompiledFilterHandle> filters)
{
    LayerRecord* source_layer = layer_system.layer_for_handle(source);
    LayerRecord* destination_layer = layer_system.layer_for_handle(destination);
    if (!source_layer || !destination_layer) {
        if (ctx.fail_frame) {
            ctx.fail_frame("CompositeLayers received invalid layer handles");
        }
        return;
    }
    if (ctx.direct_base_requested && size_t(destination) == 0 && !filters.empty()) {
        if (ctx.root_requires_preservation) {
            *ctx.root_requires_preservation = true;
        }
        if (ctx.fail_frame) {
            ctx.fail_frame("CompositeLayers root filters require offscreen presentation");
        }
        return;
    }
    if (!ctx.filter_pipeline || !ctx.recorded_content_bounds || !ctx.materialize_layer ||
        !ctx.ensure_target || !ctx.composite) {
        return;
    }
    if (ctx.scissor_state.enabled) {
        const Rml::Rectanglei scissor =
            clamp_scissor_to_surface(ctx.scissor_state.region, ctx.surface);
        if (scissor.Width() <= 0 || scissor.Height() <= 0) {
            return;
        }
    }

    FbRect source_required = ctx.recorded_content_bounds(*source_layer);
    const FilterExpansion expansion = ctx.filter_pipeline->expansion_for(ctx.filter_context, filters);
    if (!is_empty(source_required)) {
        source_required = clamp_to_surface(
            align_outward_for_render_target(expand_bounds(source_required, expansion)), ctx.surface);
    }

    if (!ctx.materialize_layer(source, source_required)) {
        if (ctx.fail_frame) {
            ctx.fail_frame("CompositeLayers failed to materialize source layer");
        }
        return;
    }
    source_layer = layer_system.materialized_layer_for_handle(source, ctx.direct_base_requested);
    if (!source_layer) {
        if (ctx.fail_frame) {
            ctx.fail_frame("CompositeLayers received unmaterialized source layer");
        }
        return;
    }

    const FbRect source_valid_global =
        source_layer->has_valid_content_bounds
            ? intersect(source_layer->valid_content_bounds, source_layer->bounds.framebuffer)
            : source_layer->bounds.framebuffer;

    if (source == destination) {
        const FbRect scratch_global_bounds = source_layer->bounds.framebuffer;
        RenderTargetRecord* scratch =
            ctx.ensure_target(PostprocessTargetKind::Scratch, scratch_global_bounds);
        if (!scratch) {
            if (ctx.fail_frame) {
                ctx.fail_frame("CompositeLayers failed to create scratch target");
            }
            return;
        }
        source_layer = layer_system.materialized_layer_for_handle(source, ctx.direct_base_requested);
        destination_layer = layer_system.materialized_layer_for_handle(destination, ctx.direct_base_requested);
        if (!source_layer || !destination_layer) {
            return;
        }
        const FbRect scratch_local_bounds{0, 0, scratch->texture_width, scratch->texture_height};
        if (!ctx.composite(make_layer_composite_op(
                make_layer_texture_region(source_layer->color, source_layer->bounds.framebuffer,
                                          full_local_rect(*source_layer), source_layer->texture_width,
                                          source_layer->texture_height),
                scratch->framebuffer, Rml::BlendMode::Replace, ScissorState{false, {}}, false, 1,
                RmlUiPassKind::Copy, RmlUiPassReason::LayerScratchCopy, "RmlUi.LayerScratchCopy",
                scratch_local_bounds))) {
            if (ctx.fail_frame) {
                ctx.fail_frame("CompositeLayers scratch copy failed");
            }
            return;
        }
        const FilterApplyResult filtered = ctx.filter_pipeline->apply(
            ctx.filter_context,
            make_layer_texture_region(
                scratch->color, source_valid_global,
                LocalFbRect{source_valid_global.x - source_layer->bounds.framebuffer.x,
                            source_valid_global.y - source_layer->bounds.framebuffer.y,
                            source_valid_global.w, source_valid_global.h},
                scratch->texture_width, scratch->texture_height),
            source_layer->bounds, filters);
        if (!bgfx::isValid(filtered.output.texture)) {
            return;
        }
        destination_layer =
            layer_system.materialized_layer_for_handle(destination, ctx.direct_base_requested);
        if (!destination_layer) {
            return;
        }
        const bool destination_clip = destination_layer->clip_mask_enabled;
        const uint8_t destination_stencil_ref = destination_layer->stencil_ref;
        const ScissorState destination_scissor =
            scissor_local_to_layer(ctx.scissor_state, destination_layer->bounds);
        const FbRect destination_bounds =
            local_rect_for_layer(filtered.output_bounds.framebuffer, *destination_layer);
        if (is_empty(destination_bounds)) {
            return;
        }
        if (!ctx.composite(make_layer_composite_op(
                filtered.output, destination_layer->framebuffer, blend_mode, destination_scissor,
                destination_clip, destination_stencil_ref, RmlUiPassKind::LayerComposite,
                RmlUiPassReason::LayerComposite, "RmlUi.LayerComposite", destination_bounds,
                filtered.composite_filter))) {
            if (ctx.fail_frame) {
                ctx.fail_frame("CompositeLayers composite failed");
            }
            return;
        }
        return;
    }

    const FilterApplyResult filtered = ctx.filter_pipeline->apply(
        ctx.filter_context,
        make_layer_texture_region(source_layer->color, source_valid_global,
                                  local_rect_for_layer(source_valid_global, *source_layer),
                                  source_layer->texture_width, source_layer->texture_height),
        source_layer->bounds, filters);
    if (!bgfx::isValid(filtered.output.texture)) {
        return;
    }

    if (!ctx.materialize_layer(destination, filtered.output_bounds.framebuffer)) {
        if (ctx.fail_frame) {
            ctx.fail_frame("CompositeLayers failed to materialize destination layer");
        }
        return;
    }
    destination_layer = layer_system.materialized_layer_for_handle(destination, ctx.direct_base_requested);
    if (!destination_layer) {
        if (ctx.fail_frame) {
            ctx.fail_frame("CompositeLayers received unmaterialized destination layer");
        }
        return;
    }
    const bool destination_clip = destination_layer->clip_mask_enabled;
    const uint8_t destination_stencil_ref = destination_layer->stencil_ref;
    const ScissorState destination_scissor =
        scissor_local_to_layer(ctx.scissor_state, destination_layer->bounds);
    const FbRect destination_bounds =
        local_rect_for_layer(filtered.output_bounds.framebuffer, *destination_layer);
    if (is_empty(destination_bounds)) {
        return;
    }
    if (!ctx.composite(make_layer_composite_op(
            filtered.output, destination_layer->framebuffer, blend_mode, destination_scissor,
            destination_clip, destination_stencil_ref, RmlUiPassKind::LayerComposite,
            RmlUiPassReason::LayerComposite, "RmlUi.LayerComposite", destination_bounds,
            filtered.composite_filter))) {
        if (ctx.fail_frame) {
            ctx.fail_frame("CompositeLayers composite failed");
        }
    }
}

} // namespace rmlui_bgfx
