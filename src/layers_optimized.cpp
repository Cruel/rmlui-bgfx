#include "rmlui_bgfx_layer_paths.hpp"

#include "rmlui_bgfx_layer_composite_helpers.hpp"

#include <span>
#include <cstdio>

namespace rmlui_bgfx {

namespace {

bool layer_save_texture_contract_bounds(const LayerRecord& layer)
{
    if (!layer.push_scissor.enabled) {
        return false;
    }
    const Rml::Rectanglei& scissor = layer.push_scissor.region;
    return scissor.Left() == layer.bounds.framebuffer.x &&
           scissor.Top() == layer.bounds.framebuffer.y &&
           scissor.Width() == layer.bounds.framebuffer.w &&
           scissor.Height() == layer.bounds.framebuffer.h;
}

BgfxFilterPipelineContext filter_context_for_source(const BgfxLayerCompositeContext& ctx,
                                                    const LayerRecord& source_layer)
{
    BgfxFilterPipelineContext filter_context = ctx.filter_context;
    filter_context.clamp_work_bounds_to_source = layer_save_texture_contract_bounds(source_layer);
    return filter_context;
}

void trace_rect(const char* label, FbRect rect)
{
    std::fprintf(stderr, " %s=(%d,%d %dx%d)", label, rect.x, rect.y, rect.w, rect.h);
}

void trace_layer_state(const BgfxLayerCompositeContext& ctx, const char* stage,
                       Rml::LayerHandle source, Rml::LayerHandle destination,
                       const LayerRecord& source_layer, const LayerRecord& destination_layer,
                       FbRect required_bounds, Rml::Span<const Rml::CompiledFilterHandle> filters)
{
    if (!ctx.filter_context.trace_filter_pipeline || filters.empty()) {
        return;
    }
    std::fprintf(
        stderr,
        "[rmlui-bgfx][optimized-layer] %s src=%zu dst=%zu filters=%zu src_kind=%d dst_kind=%d "
        "src_recording=%d src_materialized=%d src_transform=%d dst_transform=%d src_clip=%d "
        "src_clips=%zu dst_clip=%d dst_ref=%u",
        stage, size_t(source), size_t(destination), size_t(filters.size()), int(source_layer.kind),
        int(destination_layer.kind), source_layer.recording ? 1 : 0,
        source_layer.materialized ? 1 : 0, source_layer.push_transform_valid ? 1 : 0,
        destination_layer.push_transform_valid ? 1 : 0, source_layer.clip_mask_enabled ? 1 : 0,
        source_layer.clip_commands.size(), destination_layer.clip_mask_enabled ? 1 : 0,
        unsigned(destination_layer.stencil_ref));
    trace_rect("required", required_bounds);
    trace_rect("src_bounds", source_layer.bounds.framebuffer);
    trace_rect("src_valid", source_layer.valid_content_bounds);
    trace_rect("dst_bounds", destination_layer.bounds.framebuffer);
    if (ctx.scissor_state.enabled) {
        trace_rect("scissor",
                   FbRect{ctx.scissor_state.region.Left(), ctx.scissor_state.region.Top(),
                          ctx.scissor_state.region.Width(), ctx.scissor_state.region.Height()});
    }
    std::fprintf(stderr, "\n");
}

void add_valid_content_bounds(LayerRecord& layer, FbRect bounds)
{
    bounds = intersect(bounds, layer.bounds.framebuffer);
    if (is_empty(bounds)) {
        return;
    }
    layer.valid_content_bounds =
        layer.has_valid_content_bounds ? union_rects(layer.valid_content_bounds, bounds) : bounds;
    layer.has_valid_content_bounds = true;
}

} // namespace

void composite_layers_optimized(BgfxLayerSystem& layer_system, const BgfxLayerCompositeContext& ctx,
                                Rml::LayerHandle source, Rml::LayerHandle destination,
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
    if (ctx.direct_base_requested && !filters.empty() &&
        (size_t(source) == 0 || size_t(destination) == 0)) {
        if (ctx.root_requires_preservation) {
            *ctx.root_requires_preservation = true;
        }
        if (ctx.fail_frame) {
            ctx.fail_frame(nullptr);
        }
        return;
    }
    if (!ctx.filter_pipeline || !ctx.recorded_content_bounds || !ctx.materialize_layer ||
        !ctx.ensure_target || !ctx.composite) {
        return;
    }
    const std::vector<FilterRecord> resolved_filters =
        ctx.filter_pipeline->resolve(ctx.filter_context, filters);
    const bool has_effective_filters = !resolved_filters.empty();
    const bool has_filter_contract = !filters.empty();
    if (ctx.scissor_state.enabled) {
        const Rml::Rectanglei scissor =
            clamp_scissor_to_surface(ctx.scissor_state.region, ctx.surface);
        if (scissor.Width() <= 0 || scissor.Height() <= 0) {
            return;
        }
    }

    FbRect source_required = ctx.recorded_content_bounds(*source_layer);
    if (has_filter_contract) {
        if (ctx.scissor_state.enabled) {
            const Rml::Rectanglei scissor =
                clamp_scissor_to_surface(ctx.scissor_state.region, ctx.surface);
            source_required = {scissor.Left(), scissor.Top(), scissor.Width(), scissor.Height()};
        } else {
            source_required = {0, 0, ctx.surface.framebuffer_width, ctx.surface.framebuffer_height};
        }
    }
    const FilterExpansion expansion =
        has_effective_filters ? ctx.filter_pipeline->expansion_for(ctx.filter_context, filters)
                              : FilterExpansion{};
    if (!has_filter_contract && !is_empty(source_required)) {
        source_required = clamp_to_surface(
            align_outward_for_render_target(expand_bounds(source_required, expansion)),
            ctx.surface);
    } else if (has_filter_contract) {
        source_required =
            clamp_to_surface(align_outward_for_render_target(source_required), ctx.surface);
    }
    bool source_required_is_root_transform_scissor = false;
    if (has_effective_filters && source_layer->push_transform_valid && ctx.scissor_state.enabled &&
        size_t(destination) == 0) {
        const Rml::Rectanglei scissor =
            clamp_scissor_to_surface(ctx.scissor_state.region, ctx.surface);
        if (scissor.Width() > 0 && scissor.Height() > 0) {
            source_required = {scissor.Left(), scissor.Top(), scissor.Width(), scissor.Height()};
            source_required_is_root_transform_scissor = true;
        }
    }

    trace_layer_state(ctx, "before-materialize", source, destination, *source_layer,
                      *destination_layer, source_required,
                      has_effective_filters ? filters
                                            : Rml::Span<const Rml::CompiledFilterHandle>());

    const bool source_was_materialized = source_layer->materialized;
    const GlobalFbRect saved_source_valid = source_layer->valid_content_bounds;
    const bool saved_source_has_valid = source_layer->has_valid_content_bounds;
    const bool source_recorded_is_complete = !source_was_materialized;
    if (source_required_is_root_transform_scissor) {
        // The reference renderer composites filtered transformed layers from the current save/work
        // rectangle, not from the union of all recorded transformed decorator geometry. Restrict
        // the materialized source to the same contract before replay so sibling/decorator bounds do
        // not widen the source texture used by the filter pipeline.
        source_layer->valid_content_bounds = source_required;
        source_layer->has_valid_content_bounds = true;
    }

    if (!ctx.materialize_layer(source, source_required)) {
        if (ctx.fail_frame) {
            ctx.fail_frame("CompositeLayers failed to materialize source layer");
        }
        return;
    }
    source_layer = layer_system.materialized_layer_for_handle(source, ctx.direct_base_requested);
    if (source_layer && source_required_is_root_transform_scissor) {
        source_layer->valid_content_bounds = saved_source_valid;
        source_layer->has_valid_content_bounds = saved_source_has_valid;
    }
    if (!source_layer) {
        if (ctx.fail_frame) {
            ctx.fail_frame("CompositeLayers received unmaterialized source layer");
        }
        return;
    }
    destination_layer =
        layer_system.materialized_layer_for_handle(destination, ctx.direct_base_requested);
    if (!destination_layer) {
        destination_layer = layer_system.layer_for_handle(destination);
    }
    if (destination_layer) {
        trace_layer_state(ctx, "after-materialize", source, destination, *source_layer,
                          *destination_layer, source_required, filters);
    }

    // Unfiltered composites can use tight recorded content bounds. Filtered composites must keep
    // the materialized layer rectangle as the source image contract: generated callback textures
    // such as inset box-shadow depend on transparent margins inside the layer, and trimming them
    // shifts the filtered result relative to the geometry that later samples the saved texture.
    // If a filter property was present, keep the filter/window allocation contract separate from
    // the actual source pixels. No-op filter chains still need the wider RmlUi layer contract, but
    // sampling should stay limited to content that can contribute pixels.
    FbRect source_valid_global =
        source_recorded_is_complete && source_layer->has_valid_content_bounds
            ? intersect(source_layer->valid_content_bounds, source_layer->bounds.framebuffer)
            : source_layer->bounds.framebuffer;
    if (has_filter_contract) {
        source_valid_global = source_layer->has_valid_content_bounds
                                  ? intersect(source_layer->valid_content_bounds, source_required)
                                  : source_required;
    }
    const RenderBounds filter_source_bounds =
        source_required_is_root_transform_scissor
            ? RenderBounds{framebuffer_to_logical(source_required, ctx.surface), source_required}
            : source_layer->bounds;

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
        source_layer =
            layer_system.materialized_layer_for_handle(source, ctx.direct_base_requested);
        destination_layer =
            layer_system.materialized_layer_for_handle(destination, ctx.direct_base_requested);
        if (!source_layer || !destination_layer) {
            return;
        }
        const FbRect scratch_local_bounds{0, 0, scratch->texture_width, scratch->texture_height};
        if (!ctx.composite(make_layer_composite_op(
                make_layer_texture_region(source_layer->color, source_layer->bounds.framebuffer,
                                          full_local_rect(*source_layer),
                                          source_layer->texture_width,
                                          source_layer->texture_height),
                scratch->framebuffer, Rml::BlendMode::Replace, ScissorState{false, {}}, false, 1,
                RmlUiPassKind::Copy, RmlUiPassReason::LayerScratchCopy, "RmlUi.LayerScratchCopy",
                scratch_local_bounds))) {
            if (ctx.fail_frame) {
                ctx.fail_frame("CompositeLayers scratch copy failed");
            }
            return;
        }
        BgfxFilterPipelineContext source_filter_context =
            filter_context_for_source(ctx, *source_layer);
        if (source_required_is_root_transform_scissor) {
            source_filter_context.clamp_work_bounds_to_source = true;
        }
        const FilterApplyResult filtered = ctx.filter_pipeline->apply(
            source_filter_context,
            subregion(make_layer_texture_region(
                          scratch->color, source_layer->bounds.framebuffer,
                          LocalFbRect{0, 0, scratch->texture_width, scratch->texture_height},
                          scratch->texture_width, scratch->texture_height),
                      source_valid_global),
            filter_source_bounds,
            has_effective_filters ? filters : Rml::Span<const Rml::CompiledFilterHandle>());
        if (!bgfx::isValid(filtered.output.texture)) {
            return;
        }
        if (ctx.filter_context.trace_filter_pipeline && has_effective_filters) {
            std::fprintf(stderr,
                         "[rmlui-bgfx][optimized-layer] filtered-self src=%zu dst=%zu out_tex=%u "
                         "out_size=%dx%d",
                         size_t(source), size_t(destination),
                         bgfx::isValid(filtered.output.texture) ? filtered.output.texture.idx
                                                                : 65535u,
                         filtered.output.texture_width, filtered.output.texture_height);
            trace_rect("out_global", filtered.output.global_bounds);
            trace_rect("out_local", filtered.output.local_rect);
            trace_rect("out_bounds", filtered.output_bounds.framebuffer);
            trace_rect("valid_out", filtered.valid_output_bounds.framebuffer);
            std::fprintf(stderr, "\n");
        }
        destination_layer =
            layer_system.materialized_layer_for_handle(destination, ctx.direct_base_requested);
        if (!destination_layer) {
            return;
        }
        bool destination_clip = destination_layer->clip_mask_enabled;
        uint8_t destination_stencil_ref = destination_layer->stencil_ref;
        if (source_layer->clip_mask_enabled && !source_layer->clip_commands.empty() &&
            ctx.replay_clip_commands) {
            ctx.replay_clip_commands(destination, source_layer->clip_commands);
            destination_layer =
                layer_system.materialized_layer_for_handle(destination, ctx.direct_base_requested);
            if (!destination_layer) {
                return;
            }
            destination_clip = true;
            destination_stencil_ref = destination_layer->stencil_ref;
        }
        const ScissorState destination_local_scissor =
            scissor_local_to_layer(ctx.scissor_state, destination_layer->bounds);
        const FbRect destination_local_bounds =
            local_rect_for_layer(filtered.output_bounds.framebuffer, *destination_layer);
        if (is_empty(destination_local_bounds)) {
            return;
        }
        if (!ctx.composite(make_layer_composite_op(
                filtered.output, destination_layer->framebuffer, blend_mode,
                destination_local_scissor, destination_clip, destination_stencil_ref,
                RmlUiPassKind::LayerComposite, RmlUiPassReason::LayerComposite,
                "RmlUi.LayerComposite", destination_local_bounds, filtered.composite_filter))) {
            if (ctx.fail_frame) {
                ctx.fail_frame("CompositeLayers composite failed");
            }
            return;
        }
        add_valid_content_bounds(*destination_layer, filtered.valid_output_bounds.framebuffer);
        return;
    }

    BgfxFilterPipelineContext source_filter_context = filter_context_for_source(ctx, *source_layer);
    if (source_required_is_root_transform_scissor) {
        source_filter_context.clamp_work_bounds_to_source = true;
    }
    const FilterApplyResult filtered = ctx.filter_pipeline->apply(
        source_filter_context,
        subregion(make_layer_texture_region(source_layer->color, source_layer->bounds.framebuffer,
                                            full_local_rect(*source_layer),
                                            source_layer->texture_width,
                                            source_layer->texture_height),
                  source_valid_global),
        filter_source_bounds,
        has_effective_filters ? filters : Rml::Span<const Rml::CompiledFilterHandle>());
    if (!bgfx::isValid(filtered.output.texture)) {
        return;
    }
    if (ctx.filter_context.trace_filter_pipeline && has_effective_filters) {
        std::fprintf(
            stderr,
            "[rmlui-bgfx][optimized-layer] filtered src=%zu dst=%zu out_tex=%u out_size=%dx%d",
            size_t(source), size_t(destination),
            bgfx::isValid(filtered.output.texture) ? filtered.output.texture.idx : 65535u,
            filtered.output.texture_width, filtered.output.texture_height);
        trace_rect("out_global", filtered.output.global_bounds);
        trace_rect("out_local", filtered.output.local_rect);
        trace_rect("out_bounds", filtered.output_bounds.framebuffer);
        trace_rect("valid_out", filtered.valid_output_bounds.framebuffer);
        std::fprintf(stderr, "\n");
    }

    {
        LayerRecord* dst = layer_system.layer_for_handle(destination);
        if (!dst) {
            dst =
                layer_system.materialized_layer_for_handle(destination, ctx.direct_base_requested);
        }
        const FbRect dst_bounds =
            dst ? union_rects(dst->bounds.framebuffer, filtered.output_bounds.framebuffer)
                : filtered.output_bounds.framebuffer;
        if (!ctx.materialize_layer(destination, dst_bounds)) {
            if (ctx.fail_frame) {
                ctx.fail_frame("CompositeLayers failed to materialize destination layer");
            }
            return;
        }
    }
    destination_layer =
        layer_system.materialized_layer_for_handle(destination, ctx.direct_base_requested);
    if (!destination_layer) {
        if (ctx.fail_frame) {
            ctx.fail_frame("CompositeLayers received unmaterialized destination layer");
        }
        return;
    }
    bool destination_clip = destination_layer->clip_mask_enabled;
    uint8_t destination_stencil_ref = destination_layer->stencil_ref;
    if (source_layer->clip_mask_enabled && !source_layer->clip_commands.empty() &&
        ctx.replay_clip_commands) {
        ctx.replay_clip_commands(destination, source_layer->clip_commands);
        destination_layer =
            layer_system.materialized_layer_for_handle(destination, ctx.direct_base_requested);
        if (!destination_layer) {
            return;
        }
        destination_clip = true;
        destination_stencil_ref = destination_layer->stencil_ref;
    }
    const ScissorState destination_local_scissor =
        scissor_local_to_layer(ctx.scissor_state, destination_layer->bounds);
    // Filter output bounds are global framebuffer coordinates. Convert them exactly once into the
    // destination layer's target-local rectangle before building CompositeOp.
    const FbRect destination_local_bounds =
        local_rect_for_layer(filtered.output_bounds.framebuffer, *destination_layer);
    if (ctx.filter_context.trace_filter_pipeline && has_effective_filters) {
        std::fprintf(
            stderr,
            "[rmlui-bgfx][optimized-layer] composite src=%zu dst=%zu dst_clip=%d dst_ref=%u",
            size_t(source), size_t(destination), destination_clip ? 1 : 0,
            unsigned(destination_stencil_ref));
        trace_rect("dst_local", destination_local_bounds);
        if (destination_local_scissor.enabled) {
            trace_rect("dst_scissor_local", FbRect{destination_local_scissor.region.Left(),
                                                   destination_local_scissor.region.Top(),
                                                   destination_local_scissor.region.Width(),
                                                   destination_local_scissor.region.Height()});
        }
        std::fprintf(stderr, "\n");
    }
    if (is_empty(destination_local_bounds)) {
        return;
    }
    if (!ctx.composite(make_layer_composite_op(
            filtered.output, destination_layer->framebuffer, blend_mode, destination_local_scissor,
            destination_clip, destination_stencil_ref, RmlUiPassKind::LayerComposite,
            RmlUiPassReason::LayerComposite, "RmlUi.LayerComposite", destination_local_bounds,
            filtered.composite_filter))) {
        if (ctx.fail_frame) {
            ctx.fail_frame("CompositeLayers composite failed");
        }
        return;
    }
    add_valid_content_bounds(*destination_layer, filtered.valid_output_bounds.framebuffer);
}

} // namespace rmlui_bgfx
