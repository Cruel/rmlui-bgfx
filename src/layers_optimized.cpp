#include "rmlui_bgfx_layer_paths.hpp"

#include "rmlui_bgfx_layer_composite_helpers.hpp"
#include "rmlui_bgfx_trace.hpp"

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

void trace_layer_state(const BgfxLayerCompositeContext& ctx, const char* stage,
                       Rml::LayerHandle source, Rml::LayerHandle destination,
                       const LayerRecord& source_layer, const LayerRecord& destination_layer,
                       FbRect required_bounds, Rml::Span<const Rml::CompiledFilterHandle> filters)
{
    RMLUI_BGFX_TRACE(ctx.trace, TraceCategory::Layer, stage, "CompositeLayers", {
        line.field("src", size_t(source));
        line.field("dst", size_t(destination));
        line.field("filters", filters.size());
        line.field("src_kind", int(source_layer.kind));
        line.field("dst_kind", int(destination_layer.kind));
        line.field("src_recording", source_layer.recording);
        line.field("src_materialized", source_layer.materialized);
        line.field("src_transform", source_layer.push_transform_valid);
        line.field("dst_transform", destination_layer.push_transform_valid);
        line.field("src_clip", source_layer.clip_mask_enabled);
        line.field("src_clips", source_layer.clip_commands.size());
        line.field("dst_clip", destination_layer.clip_mask_enabled);
        line.field("dst_ref", unsigned(destination_layer.stencil_ref));
        line.fb_rect("source_required", required_bounds);
        line.fb_rect("src_bounds", source_layer.bounds.framebuffer);
        line.fb_rect("src_valid", source_layer.valid_content_bounds);
        line.fb_rect("dst_bounds", destination_layer.bounds.framebuffer);
        line.scissor("scissor", ctx.scissor_state);
    });
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

bool contains_rect(FbRect outer, FbRect inner)
{
    if (is_empty(inner)) {
        return true;
    }
    if (is_empty(outer)) {
        return false;
    }
    return inner.x >= outer.x && inner.y >= outer.y && inner.x + inner.w <= outer.x + outer.w &&
           inner.y + inner.h <= outer.y + outer.h;
}

struct PreservedLayerContents {
    RenderTargetRecord* scratch = nullptr;
    FbRect global_bounds{};
};

std::optional<PreservedLayerContents> preserve_layer_contents(const BgfxLayerCompositeContext& ctx,
                                                              const LayerRecord& layer,
                                                              FbRect required_bounds)
{
    if (!layer.materialized || !bgfx::isValid(layer.framebuffer) ||
        contains_rect(layer.bounds.framebuffer, required_bounds)) {
        return std::nullopt;
    }

    FbRect preserve_bounds = layer.has_valid_content_bounds
                                 ? intersect(layer.valid_content_bounds, layer.bounds.framebuffer)
                                 : layer.bounds.framebuffer;
    if (is_empty(preserve_bounds)) {
        return std::nullopt;
    }

    RenderTargetRecord* scratch =
        ctx.ensure_target(PostprocessTargetKind::Scratch, preserve_bounds);
    if (!scratch) {
        return PreservedLayerContents{};
    }

    const FbRect scratch_local_bounds{0, 0, scratch->texture_width, scratch->texture_height};
    if (!ctx.composite(make_layer_composite_op(
            make_layer_texture_region(layer.color, layer.bounds.framebuffer,
                                      local_rect_for_layer(preserve_bounds, layer),
                                      layer.texture_width, layer.texture_height),
            scratch->framebuffer, Rml::BlendMode::Replace, ScissorState{false, {}}, false, 1,
            RmlUiPassKind::Copy, RmlUiPassReason::LayerScratchCopy, "RmlUi.LayerPreserveCopy",
            scratch_local_bounds))) {
        return PreservedLayerContents{};
    }

    return PreservedLayerContents{scratch, preserve_bounds};
}

bool restore_layer_contents(const BgfxLayerCompositeContext& ctx,
                            const PreservedLayerContents& saved,
                            const LayerRecord& destination_layer)
{
    if (!saved.scratch || !bgfx::isValid(saved.scratch->color)) {
        return true;
    }

    const FbRect destination_local_bounds =
        local_rect_for_layer(saved.global_bounds, destination_layer);
    if (is_empty(destination_local_bounds)) {
        return true;
    }

    return ctx.composite(make_layer_composite_op(
        make_layer_texture_region(
            saved.scratch->color, saved.global_bounds,
            LocalFbRect{0, 0, saved.scratch->texture_width, saved.scratch->texture_height},
            saved.scratch->texture_width, saved.scratch->texture_height),
        destination_layer.framebuffer, Rml::BlendMode::Replace, ScissorState{false, {}}, false, 1,
        RmlUiPassKind::Copy, RmlUiPassReason::LayerScratchCopy, "RmlUi.LayerPreserveRestore",
        destination_local_bounds));
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
        RMLUI_BGFX_TRACE_FAILURE(ctx.trace, "CompositeLayers", "invalid layer handles", {
            line.field("src", size_t(source));
            line.field("dst", size_t(destination));
            line.field("has_src", source_layer != nullptr);
            line.field("has_dst", destination_layer != nullptr);
        });
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
        RMLUI_BGFX_TRACE(ctx.trace, TraceCategory::Composite, "skip", "CompositeLayers", {
            line.field("src", size_t(source));
            line.field("dst", size_t(destination));
            line.field("reason", "direct base preservation required");
        });
        return;
    }
    if (!ctx.filter_pipeline || !ctx.recorded_content_bounds || !ctx.materialize_layer ||
        !ctx.ensure_target || !ctx.composite) {
        RMLUI_BGFX_TRACE_FAILURE(ctx.trace, "CompositeLayers", "missing callbacks", {
            line.field("src", size_t(source));
            line.field("dst", size_t(destination));
        });
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
            RMLUI_BGFX_TRACE(ctx.trace, TraceCategory::Composite, "skip", "CompositeLayers", {
                line.field("src", size_t(source));
                line.field("dst", size_t(destination));
                line.field("reason", "empty scissor");
                line.rml_rect("scissor", scissor);
            });
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
        RMLUI_BGFX_TRACE_FALLBACK(ctx.trace, "CompositeLayers", "root transformed filter scissor", {
            line.field("src", size_t(source));
            line.field("dst", size_t(destination));
            line.fb_rect("source_required", source_required);
        });
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
        RMLUI_BGFX_TRACE_FAILURE(ctx.trace, "CompositeLayers", "materialize source failed", {
            line.field("src", size_t(source));
            line.field("dst", size_t(destination));
            line.fb_rect("source_required", source_required);
        });
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
        RMLUI_BGFX_TRACE_FAILURE(ctx.trace, "CompositeLayers", "unmaterialized source", {
            line.field("src", size_t(source));
            line.field("dst", size_t(destination));
        });
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
    // the actual source pixels for real postprocess chains. No-op filter chains are different:
    // the pipeline will return without running a pass, so preserve the RmlUi layer contract here
    // instead of shrinking the composite to the tracked content bounds.
    const bool has_non_empty_source_content_bounds =
        source_layer->has_valid_content_bounds && !is_empty(source_layer->valid_content_bounds);
    FbRect source_valid_global =
        source_recorded_is_complete && has_non_empty_source_content_bounds
            ? intersect(source_layer->valid_content_bounds, source_layer->bounds.framebuffer)
            : source_layer->bounds.framebuffer;
    if (has_filter_contract && has_effective_filters) {
        // Match GL3/reference semantics: filters sample the active work rectangle, normally the
        // current scissor/save bounds. Do not shrink real filter input to tracked ink bounds; blur
        // and box-shadow callback textures depend on the transparent margins inside the work rect.
        source_valid_global = source_required;
    } else if (has_filter_contract) {
        source_valid_global = source_required;
    }
    const RenderBounds filter_source_bounds =
        source_required_is_root_transform_scissor
            ? RenderBounds{framebuffer_to_logical(source_required, ctx.surface), source_required}
            : source_layer->bounds;
    RMLUI_BGFX_TRACE(ctx.trace, TraceCategory::Composite, "plan", "CompositeLayersSource", {
        line.field("src", size_t(source));
        line.field("dst", size_t(destination));
        line.field("has_filter_contract", has_filter_contract);
        line.field("has_effective_filters", has_effective_filters);
        line.field("source_recorded_complete", source_recorded_is_complete);
        line.fb_rect("source_required", source_required);
        line.fb_rect("source_valid_global", source_valid_global);
        line.fb_rect("source_bounds", source_layer->bounds.framebuffer);
        line.fb_rect("filter_source_bounds", filter_source_bounds.framebuffer);
    });

    if (source == destination) {
        const FbRect scratch_global_bounds = source_layer->bounds.framebuffer;
        RenderTargetRecord* scratch =
            ctx.ensure_target(PostprocessTargetKind::Scratch, scratch_global_bounds);
        if (!scratch) {
            if (ctx.fail_frame) {
                ctx.fail_frame("CompositeLayers failed to create scratch target");
            }
            RMLUI_BGFX_TRACE_FAILURE(ctx.trace, "CompositeLayers", "scratch target failed", {
                line.field("src", size_t(source));
                line.field("dst", size_t(destination));
                line.fb_rect("scratch_bounds", scratch_global_bounds);
            });
            return;
        }
        source_layer =
            layer_system.materialized_layer_for_handle(source, ctx.direct_base_requested);
        destination_layer =
            layer_system.materialized_layer_for_handle(destination, ctx.direct_base_requested);
        if (!source_layer || !destination_layer) {
            RMLUI_BGFX_TRACE_FAILURE(ctx.trace, "CompositeLayers", "self layer missing", {
                line.field("src", size_t(source));
                line.field("dst", size_t(destination));
            });
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
            RMLUI_BGFX_TRACE_FAILURE(ctx.trace, "CompositeLayers", "scratch copy failed", {
                line.field("src", size_t(source));
                line.field("dst", size_t(destination));
                line.fb_rect("scratch_bounds", scratch_global_bounds);
                line.local_rect("scratch_local", scratch_local_bounds);
            });
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
            RMLUI_BGFX_TRACE_FAILURE(ctx.trace, "CompositeLayers", "invalid self-filter output", {
                line.field("src", size_t(source));
                line.field("dst", size_t(destination));
                line.fb_rect("source_valid_global", source_valid_global);
                line.fb_rect("filter_source_bounds", filter_source_bounds.framebuffer);
            });
            return;
        }
        RMLUI_BGFX_TRACE(ctx.trace, TraceCategory::Filter, "end", "CompositeLayersSelfFilter", {
            line.field("src", size_t(source));
            line.field("dst", size_t(destination));
            line.texture_region("output", filtered.output);
            line.fb_rect("output_bounds", filtered.output_bounds.framebuffer);
            line.fb_rect("valid_output", filtered.valid_output_bounds.framebuffer);
        });
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
        const FbRect destination_local_bounds = local_rect_for_layer(
            final_filter_composite_global_bounds(filtered), *destination_layer);
        if (is_empty(destination_local_bounds)) {
            RMLUI_BGFX_TRACE_FAILURE(ctx.trace, "CompositeLayers", "empty self destination rect", {
                line.field("src", size_t(source));
                line.field("dst", size_t(destination));
                line.fb_rect("filtered_global", final_filter_composite_global_bounds(filtered));
                line.fb_rect("dst_bounds", destination_layer->bounds.framebuffer);
            });
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
            RMLUI_BGFX_TRACE_FAILURE(ctx.trace, "CompositeLayers", "self composite failed", {
                line.field("src", size_t(source));
                line.field("dst", size_t(destination));
                line.local_rect("destination", destination_local_bounds);
            });
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
        RMLUI_BGFX_TRACE_FAILURE(ctx.trace, "CompositeLayers", "invalid filter output", {
            line.field("src", size_t(source));
            line.field("dst", size_t(destination));
            line.fb_rect("source_valid_global", source_valid_global);
            line.fb_rect("filter_source_bounds", filter_source_bounds.framebuffer);
        });
        return;
    }
    RMLUI_BGFX_TRACE(ctx.trace, TraceCategory::Filter, "end", "CompositeLayersFilter", {
        line.field("src", size_t(source));
        line.field("dst", size_t(destination));
        line.texture_region("output", filtered.output);
        line.fb_rect("output_bounds", filtered.output_bounds.framebuffer);
        line.fb_rect("valid_output", filtered.valid_output_bounds.framebuffer);
    });

    {
        LayerRecord* dst = layer_system.layer_for_handle(destination);
        if (!dst) {
            dst =
                layer_system.materialized_layer_for_handle(destination, ctx.direct_base_requested);
        }
        const bool use_existing_destination_bounds =
            dst && (!dst->recording || dst->materialized || !dst->commands.empty());
        const FbRect dst_bounds =
            use_existing_destination_bounds
                ? union_rects(dst->bounds.framebuffer, filtered.output_bounds.framebuffer)
                : filtered.output_bounds.framebuffer;
        std::optional<PreservedLayerContents> preserved_destination;
        if (dst) {
            preserved_destination = preserve_layer_contents(ctx, *dst, dst_bounds);
            if (preserved_destination && !preserved_destination->scratch) {
                if (ctx.fail_frame) {
                    ctx.fail_frame("CompositeLayers failed to preserve destination layer");
                }
                RMLUI_BGFX_TRACE_FAILURE(ctx.trace, "CompositeLayers",
                                         "preserve destination failed", {
                                             line.field("dst", size_t(destination));
                                             line.fb_rect("dst_bounds", dst_bounds);
                                         });
                return;
            }
        }
        if (!ctx.materialize_layer(destination, dst_bounds)) {
            if (ctx.fail_frame) {
                ctx.fail_frame("CompositeLayers failed to materialize destination layer");
            }
            RMLUI_BGFX_TRACE_FAILURE(ctx.trace, "CompositeLayers",
                                     "materialize destination failed", {
                                         line.field("dst", size_t(destination));
                                         line.fb_rect("dst_bounds", dst_bounds);
                                     });
            return;
        }
        if (preserved_destination) {
            LayerRecord* materialized_destination =
                layer_system.materialized_layer_for_handle(destination, ctx.direct_base_requested);
            if (!materialized_destination ||
                !restore_layer_contents(ctx, *preserved_destination, *materialized_destination)) {
                if (ctx.fail_frame) {
                    ctx.fail_frame("CompositeLayers failed to restore destination layer");
                }
                RMLUI_BGFX_TRACE_FAILURE(ctx.trace, "CompositeLayers",
                                         "restore destination failed", {
                                             line.field("dst", size_t(destination));
                                             line.fb_rect("dst_bounds", dst_bounds);
                                         });
                return;
            }
        }
    }
    destination_layer =
        layer_system.materialized_layer_for_handle(destination, ctx.direct_base_requested);
    if (!destination_layer) {
        if (ctx.fail_frame) {
            ctx.fail_frame("CompositeLayers received unmaterialized destination layer");
        }
        RMLUI_BGFX_TRACE_FAILURE(ctx.trace, "CompositeLayers", "unmaterialized destination", {
            line.field("dst", size_t(destination));
        });
        return;
    }
    bool destination_clip = destination_layer->clip_mask_enabled;
    uint8_t destination_stencil_ref = destination_layer->stencil_ref;
    if (source_layer->clip_mask_enabled && !source_layer->clip_commands.empty() &&
        ctx.replay_clip_commands) {
        ctx.replay_clip_commands(destination, source_layer->clip_commands);
        RMLUI_BGFX_TRACE(ctx.trace, TraceCategory::Clip, "replay", "CompositeLayersSourceClips", {
            line.field("src", size_t(source));
            line.field("dst", size_t(destination));
            line.field("count", source_layer->clip_commands.size());
        });
        destination_layer =
            layer_system.materialized_layer_for_handle(destination, ctx.direct_base_requested);
        if (!destination_layer) {
            RMLUI_BGFX_TRACE_FAILURE(ctx.trace, "CompositeLayers", "destination missing after clips", {
                line.field("dst", size_t(destination));
            });
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
        local_rect_for_layer(final_filter_composite_global_bounds(filtered), *destination_layer);
    RMLUI_BGFX_TRACE(ctx.trace, TraceCategory::Composite, "plan", "CompositeLayers", {
        line.field("src", size_t(source));
        line.field("dst", size_t(destination));
        line.field("dst_clip", destination_clip);
        line.field("dst_ref", unsigned(destination_stencil_ref));
        line.local_rect("dst_local", destination_local_bounds);
        line.scissor("dst_scissor_local", destination_local_scissor);
        line.fb_rect("filtered_global", final_filter_composite_global_bounds(filtered));
        line.fb_rect("dst_bounds", destination_layer->bounds.framebuffer);
    });
    if (is_empty(destination_local_bounds)) {
        RMLUI_BGFX_TRACE_FAILURE(ctx.trace, "CompositeLayers", "empty destination rect", {
            line.field("src", size_t(source));
            line.field("dst", size_t(destination));
            line.fb_rect("filtered_global", final_filter_composite_global_bounds(filtered));
            line.fb_rect("dst_bounds", destination_layer->bounds.framebuffer);
        });
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
        RMLUI_BGFX_TRACE_FAILURE(ctx.trace, "CompositeLayers", "composite failed", {
            line.field("src", size_t(source));
            line.field("dst", size_t(destination));
            line.local_rect("destination", destination_local_bounds);
        });
        return;
    }
    add_valid_content_bounds(*destination_layer, filtered.valid_output_bounds.framebuffer);
}

} // namespace rmlui_bgfx
