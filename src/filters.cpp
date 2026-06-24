#include "rmlui_bgfx_filters.hpp"

#include <algorithm>

namespace rmlui_bgfx {

namespace {

[[nodiscard]] TextureRegion texture_region(bgfx::TextureHandle texture, GlobalFbRect global_bounds,
                                           LocalFbRect local_rect, int texture_width,
                                           int texture_height)
{
    return TextureRegion{texture, global_bounds, local_rect, texture_width, texture_height};
}

[[nodiscard]] CompositeOp make_composite_op(TextureRegion source,
                                            bgfx::FrameBufferHandle destination,
                                            Rml::BlendMode blend_mode, ScissorState scissor,
                                            bool apply_destination_stencil, uint8_t stencil_ref,
                                            RmlUiPassKind kind, RmlUiPassReason reason,
                                            const char* name, LocalFbRect destination_rect = {})
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
    return op;
}

[[nodiscard]] bool is_full_frame_surface(FbRect rect, const SurfaceMetrics& surface)
{
    return !is_empty(rect) && rect.x == 0 && rect.y == 0 && rect.w >= surface.framebuffer_width &&
           rect.h >= surface.framebuffer_height;
}

[[nodiscard]] FbRect clamp_valid_filter_bounds(FbRect bounds, FbRect allocation_bounds)
{
    return intersect(bounds, allocation_bounds);
}

[[nodiscard]] FbRect advance_valid_filter_bounds(FbRect current, const FilterRecord& filter,
                                                 FbRect allocation_bounds)
{
    switch (filter.kind) {
    case FilterKind::Blur:
        return clamp_valid_filter_bounds(expand_bounds(current, blur_expansion(filter.sigma)),
                                         allocation_bounds);
    case FilterKind::DropShadow:
        return clamp_valid_filter_bounds(
            expand_bounds(current,
                          drop_shadow_expansion(filter.sigma, filter.offset[0], filter.offset[1])),
            allocation_bounds);
    case FilterKind::Opacity:
    case FilterKind::ColorMatrix:
    case FilterKind::MaskImage:
    case FilterKind::Invalid:
        return clamp_valid_filter_bounds(current, allocation_bounds);
    }
    return clamp_valid_filter_bounds(current, allocation_bounds);
}

[[nodiscard]] RenderBounds render_bounds_from_framebuffer(FbRect bounds,
                                                          const SurfaceMetrics& surface)
{
    return {framebuffer_to_logical(bounds, surface), bounds};
}

[[nodiscard]] uint32_t stencil_test_state_for_ref(uint8_t ref)
{
    return BGFX_STENCIL_TEST_EQUAL | BGFX_STENCIL_FUNC_REF(uint32_t(ref)) |
           BGFX_STENCIL_FUNC_RMASK(0xff) | BGFX_STENCIL_OP_FAIL_S_KEEP |
           BGFX_STENCIL_OP_FAIL_Z_KEEP | BGFX_STENCIL_OP_PASS_Z_KEEP;
}

} // namespace

std::vector<FilterRecord>
BgfxFilterPipeline::resolve(const BgfxFilterPipelineContext& ctx,
                            Rml::Span<const Rml::CompiledFilterHandle> filter_handles) const
{
    std::vector<FilterRecord> filter_chain;
    filter_chain.reserve(filter_handles.size());
    for (Rml::CompiledFilterHandle handle : filter_handles) {
        auto it = ctx.filters.find(handle);
        if (it == ctx.filters.end()) {
            return {};
        }
        filter_chain.push_back(it->second);
    }
    return simplify_filter_chain(filter_chain);
}

FilterExpansion
BgfxFilterPipeline::expansion_for(const BgfxFilterPipelineContext& ctx,
                                  Rml::Span<const Rml::CompiledFilterHandle> filter_handles) const
{
    FilterExpansion total_expansion{};
    for (const FilterRecord& filter : resolve(ctx, filter_handles)) {
        switch (filter.kind) {
        case FilterKind::Blur:
            total_expansion = add_expansions(total_expansion, blur_expansion(filter.sigma));
            break;
        case FilterKind::DropShadow:
            total_expansion = add_expansions(
                total_expansion,
                drop_shadow_expansion(filter.sigma, filter.offset[0], filter.offset[1]));
            break;
        case FilterKind::MaskImage:
        case FilterKind::Opacity:
        case FilterKind::ColorMatrix:
        case FilterKind::Invalid:
            break;
        }
    }
    return total_expansion;
}

bool BgfxFilterPipeline::texture_attached_to_framebuffer(const BgfxFilterPipelineContext& ctx,
                                                         bgfx::TextureHandle texture,
                                                         bgfx::FrameBufferHandle framebuffer) const
{
    for (const RenderTargetRecord& target : ctx.target_cache.postprocess_targets()) {
        if (bgfx::isValid(target.framebuffer) && target.framebuffer.idx == framebuffer.idx &&
            bgfx::isValid(target.color) && target.color.idx == texture.idx) {
            return true;
        }
    }
    for (const LayerRecord& layer : ctx.target_cache.layers()) {
        if (bgfx::isValid(layer.framebuffer) && layer.framebuffer.idx == framebuffer.idx &&
            bgfx::isValid(layer.color) && layer.color.idx == texture.idx) {
            return true;
        }
    }
    return false;
}

RenderTargetRecord* BgfxFilterPipeline::safe_destination(const BgfxFilterPipelineContext& ctx,
                                                         bgfx::TextureHandle source,
                                                         RenderTargetRecord* current,
                                                         RenderTargetRecord* other) const
{
    if (!current) {
        return other;
    }
    if (bgfx::isValid(source) && bgfx::isValid(current->framebuffer) &&
        texture_attached_to_framebuffer(ctx, source, current->framebuffer)) {
        return other;
    }
    return current;
}

bool BgfxFilterPipeline::composite(const BgfxFilterPipelineContext& ctx,
                                   const CompositeOp& op) const
{
    if (!ctx.ensure_fullscreen_geometry || !ctx.ensure_fullscreen_geometry() ||
        !bgfx::isValid(op.source.texture)) {
        return false;
    }

    if (bgfx::isValid(op.destination) &&
        texture_attached_to_framebuffer(ctx, op.source.texture, op.destination)) {
        if (ctx.fail_frame) {
            ctx.fail_frame("composite feedback loop");
        }
        return false;
    }

    const LocalFbRect destination_rect =
        is_empty(op.destination_rect)
            ? LocalFbRect{0, 0, ctx.surface.framebuffer_width, ctx.surface.framebuffer_height}
            : op.destination_rect;
    const LocalFbRect source_rect = op.source.local_rect;
    const bool is_full_frame = is_full_frame_surface(destination_rect, ctx.surface);
    auto pass =
        ctx.pass_builder.composite(op.destination, destination_rect, op.kind, op.name, op.reason);
    if (!pass) {
        return false;
    }
    ctx.perf.add_composite(area(destination_rect), is_full_frame);
    return ctx.draw_context.submit_composite(*pass, ctx.resources, op, source_rect,
                                             destination_rect,
                                             stencil_test_state_for_ref(op.stencil_ref));
}

FilterApplyResult
BgfxFilterPipeline::apply(const BgfxFilterPipelineContext& ctx, TextureRegion source,
                          const RenderBounds& source_bounds,
                          Rml::Span<const Rml::CompiledFilterHandle> filter_handles) const
{
    FilterApplyResult result;
    const GlobalFbRect source_valid_global_bounds =
        intersect(source.global_bounds, source_bounds.framebuffer);
    if (is_empty(source_valid_global_bounds) || !bgfx::isValid(source.texture)) {
        return {};
    }

    source.global_bounds = source_valid_global_bounds;
    source.local_rect = {source_valid_global_bounds.x - source_bounds.framebuffer.x,
                         source_valid_global_bounds.y - source_bounds.framebuffer.y,
                         source_valid_global_bounds.w, source_valid_global_bounds.h};
    result.output = source;
    result.output_bounds = render_bounds_from_framebuffer(source_valid_global_bounds, ctx.surface);
    result.valid_output_bounds = result.output_bounds;
    if (filter_handles.empty()) {
        return result;
    }

    std::vector<FilterRecord> filter_chain = resolve(ctx, filter_handles);
    if (filter_chain.empty()) {
        return result;
    }

    const ColorOnlyFilterPlan color_only_plan = plan_color_only_filter_chain(filter_chain);
    if (color_only_plan.eligible) {
        result.composite_filter.enabled = true;
        result.composite_filter.opacity = color_only_plan.opacity;
        result.composite_filter.color_matrix = color_only_plan.matrix;
        ctx.perf.add_color_filter_composite_fold();
        return result;
    }

    const FilterExpansion total_expansion = expansion_for(ctx, filter_handles);
    const FbRect expanded = expand_bounds(source_valid_global_bounds, total_expansion);
    const FbRect clamped_work_bounds =
        clamp_to_surface(align_outward_for_render_target(expanded), ctx.surface);
    if (is_empty(clamped_work_bounds)) {
        return {};
    }
    RenderTargetRecord* primary = ctx.target_cache.acquire_postprocess_target(
        PostprocessTargetKind::Primary, clamped_work_bounds, ctx.surface);
    RenderTargetRecord* secondary = ctx.target_cache.acquire_postprocess_target(
        PostprocessTargetKind::Secondary, clamped_work_bounds, ctx.surface);
    if (!primary || !secondary) {
        return {};
    }

    if (filter_chain.size() == 1 && filter_chain[0].kind == FilterKind::MaskImage) {
        auto tex_it = ctx.textures.find(Rml::TextureHandle(filter_chain[0].resource));
        if (tex_it == ctx.textures.end()) {
            return {};
        }
        RenderTargetRecord* destination = safe_destination(ctx, source.texture, primary, secondary);
        if (!destination || !bgfx::isValid(destination->framebuffer)) {
            return {};
        }
        const bool is_full_mask = is_full_frame_surface(destination->bounds, ctx.surface);
        auto pass = ctx.pass_builder.postprocess(
            destination->framebuffer, destination->texture_width, destination->texture_height,
            "RmlUi.FilterMaskImage", RmlUiPassReason::FilterMaskImage);
        if (!pass) {
            return {};
        }
        ctx.perf.add_mask(uint64_t(destination->texture_width) *
                              uint64_t(destination->texture_height),
                          is_full_mask);
        if (texture_attached_to_framebuffer(ctx, tex_it->second.handle, destination->framebuffer)) {
            return {};
        }
        const FbRect mask_bounds{filter_chain[0].mask_bounds[0], filter_chain[0].mask_bounds[1],
                                 filter_chain[0].mask_bounds[2], filter_chain[0].mask_bounds[3]};
        auto mask_transform = compute_mask_uv_transform(destination->bounds, mask_bounds);
        if (bgfx::getCaps() && bgfx::getCaps()->originBottomLeft) {
            mask_transform[1] = -mask_transform[1];
            mask_transform[3] += float(destination->bounds.h) / float(std::max(mask_bounds.h, 1));
        }
        const FbRect source_sample_local{
            source.local_rect.x + destination->bounds.x - source.global_bounds.x,
            source.local_rect.y + destination->bounds.y - source.global_bounds.y,
            destination->bounds.w,
            destination->bounds.h,
        };
        const auto source_uv = uv_rect_for_source_region(source_sample_local, source.texture_width,
                                                         source.texture_height);
        if (!ctx.draw_context.submit_mask_image(*pass, ctx.resources, source.texture,
                                                tex_it->second.handle, mask_transform, source_uv)) {
            return {};
        }
        result.output = texture_region(
            destination->color, destination->bounds,
            LocalFbRect{0, 0, destination->texture_width, destination->texture_height},
            destination->texture_width, destination->texture_height);
        result.output_bounds = render_bounds_from_framebuffer(destination->bounds, ctx.surface);
        result.valid_output_bounds = render_bounds_from_framebuffer(
            advance_valid_filter_bounds(source_valid_global_bounds, filter_chain[0],
                                        destination->bounds),
            ctx.surface);
        return result;
    }

    const FbRect source_copy_global = intersect(source_bounds.framebuffer, clamped_work_bounds);
    if (is_empty(source_copy_global)) {
        return {};
    }
    const FbRect source_copy_local{source_copy_global.x - source_bounds.framebuffer.x,
                                   source_copy_global.y - source_bounds.framebuffer.y,
                                   source_copy_global.w, source_copy_global.h};
    const FbRect copy_destination{source_copy_global.x - clamped_work_bounds.x,
                                  source_copy_global.y - clamped_work_bounds.y,
                                  source_copy_global.w, source_copy_global.h};
    if (!composite(ctx, make_composite_op(
                            texture_region(source.texture, source_copy_global, source_copy_local,
                                           source.texture_width, source.texture_height),
                            primary->framebuffer, Rml::BlendMode::Replace, ScissorState{false, {}},
                            false, 1, RmlUiPassKind::Copy, RmlUiPassReason::FilterCopy,
                            "RmlUi.FilterCopy", copy_destination))) {
        return {};
    }

    bgfx::TextureHandle current = primary->color;
    RenderTargetRecord* destination = secondary;
    FbRect current_valid_rect = copy_destination;
    FbRect current_valid_global = source_valid_global_bounds;
    for (const FilterRecord& filter : filter_chain) {
        bool ok = false;
        switch (filter.kind) {
        case FilterKind::Opacity: {
            const float opacity[4] = {filter.scalar, 0.0f, 0.0f, 0.0f};
            ok = fullscreen_filter_pass(
                ctx, current, *destination, "RmlUi.FilterOpacity",
                [&](const RmlUiPass& pass) {
                    return ctx.draw_context.submit_opacity(pass, ctx.resources, current, opacity);
                },
                RmlUiPassReason::FilterOpacity);
            current_valid_rect = {0, 0, destination->texture_width, destination->texture_height};
            break;
        }
        case FilterKind::ColorMatrix:
            ok = fullscreen_filter_pass(
                ctx, current, *destination, "RmlUi.FilterColorMatrix",
                [&](const RmlUiPass& pass) {
                    return ctx.draw_context.submit_color_matrix(pass, ctx.resources, current,
                                                                filter.matrix.data());
                },
                RmlUiPassReason::FilterColorMatrix);
            current_valid_rect = {0, 0, destination->texture_width, destination->texture_height};
            break;
        case FilterKind::MaskImage: {
            auto tex_it = ctx.textures.find(Rml::TextureHandle(filter.resource));
            if (tex_it == ctx.textures.end()) {
                return {};
            }
            if (!ctx.ensure_fullscreen_geometry || !ctx.ensure_fullscreen_geometry() ||
                !bgfx::isValid(ctx.resources.mask_multiply_program)) {
                return {};
            }
            const bool is_full_mask = is_full_frame_surface(destination->bounds, ctx.surface);
            auto pass = ctx.pass_builder.postprocess(
                destination->framebuffer, destination->texture_width, destination->texture_height,
                "RmlUi.FilterMaskImage", RmlUiPassReason::FilterMaskImage);
            if (!pass) {
                return {};
            }
            ctx.perf.add_mask(uint64_t(destination->texture_width) *
                                  uint64_t(destination->texture_height),
                              is_full_mask);
            if (texture_attached_to_framebuffer(ctx, tex_it->second.handle,
                                                destination->framebuffer)) {
                if (ctx.fail_frame) {
                    ctx.fail_frame("mask-image filter feedback loop");
                }
                return {};
            }
            const FbRect mask_bounds{filter.mask_bounds[0], filter.mask_bounds[1],
                                     filter.mask_bounds[2], filter.mask_bounds[3]};
            auto mask_transform = compute_mask_uv_transform(destination->bounds, mask_bounds);
            if (bgfx::getCaps() && bgfx::getCaps()->originBottomLeft) {
                mask_transform[1] = -mask_transform[1];
                mask_transform[3] +=
                    float(destination->bounds.h) / float(std::max(mask_bounds.h, 1));
            }
            ok = ctx.draw_context.submit_mask_image(*pass, ctx.resources, current,
                                                    tex_it->second.handle, mask_transform);
            current_valid_rect = {0, 0, destination->texture_width, destination->texture_height};
            break;
        }
        case FilterKind::Blur: {
            const GaussianKernel kernel = gaussian_kernel(filter.sigma);
            const float w0 = kernel.weights.empty() ? 1.0f : kernel.weights[0];
            const float w1 = kernel.weights.size() > 1 ? kernel.weights[1] : 0.0f;
            const float w2 = kernel.weights.size() > 2 ? kernel.weights[2] : 0.0f;
            const float w3 = kernel.weights.size() > 3 ? kernel.weights[3] : 0.0f;
            const float renorm = std::max(w0 + 2.0f * (w1 + w2 + w3), 0.000001f);
            const float weights[4] = {w0 / renorm, w1 / renorm, w2 / renorm, w3 / renorm};
            const auto bounds = uv_rect_for_source_region(
                current_valid_rect, primary->texture_width, primary->texture_height);
            float params[4] = {0.0f, 1.0f / float(std::max(destination->texture_height, 1)), 0.0f,
                               0.0f};
            if (!fullscreen_filter_pass(
                    ctx, current, *destination, "RmlUi.FilterBlurV",
                    [&](const RmlUiPass& pass) {
                        return ctx.draw_context.submit_blur(pass, ctx.resources, current, params,
                                                            weights, bounds.data());
                    },
                    RmlUiPassReason::FilterBlur)) {
                return {};
            }
            ctx.perf.add_blur();
            current = destination->color;
            destination = (destination == primary) ? secondary : primary;
            params[0] = 1.0f / float(std::max(destination->texture_width, 1));
            params[1] = 0.0f;
            if (!fullscreen_filter_pass(
                    ctx, current, *destination, "RmlUi.FilterBlurH",
                    [&](const RmlUiPass& pass) {
                        return ctx.draw_context.submit_blur(pass, ctx.resources, current, params,
                                                            weights, bounds.data());
                    },
                    RmlUiPassReason::FilterBlur)) {
                return {};
            }
            ctx.perf.add_blur();
            ok = true;
            current_valid_rect = {0, 0, destination->texture_width, destination->texture_height};
            break;
        }
        case FilterKind::DropShadow: {
            const bgfx::TextureHandle original = current;
            const float color[4] = {filter.color[0], filter.color[1], filter.color[2],
                                    filter.color[3]};
            const float offset[4] = {
                filter.offset[0] / float(std::max(destination->texture_width, 1)),
                filter.offset[1] / float(std::max(destination->texture_height, 1)), 0.0f, 0.0f};
            if (!fullscreen_filter_pass(
                    ctx, current, *destination, "RmlUi.FilterDropShadowExtract",
                    [&](const RmlUiPass& pass) {
                        return ctx.draw_context.submit_drop_shadow(pass, ctx.resources, current,
                                                                   color, offset);
                    },
                    RmlUiPassReason::FilterDropShadow)) {
                return {};
            }
            ctx.perf.add_dropshadow();
            current = destination->color;
            destination = (destination == primary) ? secondary : primary;
            if (filter.sigma >= 0.5f) {
                const GaussianKernel kernel = gaussian_kernel(filter.sigma);
                const float w0 = kernel.weights.empty() ? 1.0f : kernel.weights[0];
                const float w1 = kernel.weights.size() > 1 ? kernel.weights[1] : 0.0f;
                const float w2 = kernel.weights.size() > 2 ? kernel.weights[2] : 0.0f;
                const float w3 = kernel.weights.size() > 3 ? kernel.weights[3] : 0.0f;
                const float renorm = std::max(w0 + 2.0f * (w1 + w2 + w3), 0.000001f);
                const float weights[4] = {w0 / renorm, w1 / renorm, w2 / renorm, w3 / renorm};
                const auto bounds = uv_rect_for_source_region(
                    current_valid_rect, primary->texture_width, primary->texture_height);
                float params[4] = {0.0f, 1.0f / float(std::max(destination->texture_height, 1)),
                                   0.0f, 0.0f};
                if (!fullscreen_filter_pass(
                        ctx, current, *destination, "RmlUi.FilterDropShadowBlurV",
                        [&](const RmlUiPass& pass) {
                            return ctx.draw_context.submit_blur(pass, ctx.resources, current,
                                                                params, weights, bounds.data());
                        },
                        RmlUiPassReason::FilterBlur)) {
                    return {};
                }
                ctx.perf.add_blur();
                current = destination->color;
                destination = (destination == primary) ? secondary : primary;
                params[0] = 1.0f / float(std::max(destination->texture_width, 1));
                params[1] = 0.0f;
                if (!fullscreen_filter_pass(
                        ctx, current, *destination, "RmlUi.FilterDropShadowBlurH",
                        [&](const RmlUiPass& pass) {
                            return ctx.draw_context.submit_blur(pass, ctx.resources, current,
                                                                params, weights, bounds.data());
                        },
                        RmlUiPassReason::FilterBlur)) {
                    return {};
                }
                ctx.perf.add_blur();
                current = destination->color;
                destination = (destination == primary) ? secondary : primary;
            }
            current_valid_rect = {0, 0, destination->texture_width, destination->texture_height};
            destination = safe_destination(ctx, original, destination,
                                           (destination == primary) ? secondary : primary);
            if (!composite(ctx, make_composite_op(
                                    texture_region(original, destination->bounds,
                                                   LocalFbRect{0, 0, destination->texture_width,
                                                               destination->texture_height},
                                                   destination->texture_width,
                                                   destination->texture_height),
                                    destination->framebuffer, Rml::BlendMode::Blend,
                                    ScissorState{false, {}}, false, 1, RmlUiPassKind::Postprocess,
                                    RmlUiPassReason::FilterDropShadowComposite,
                                    "RmlUi.FilterDropShadowComposite",
                                    LocalFbRect{0, 0, destination->texture_width,
                                                destination->texture_height}))) {
                return {};
            }
            ok = true;
            current_valid_rect = {0, 0, destination->texture_width, destination->texture_height};
            break;
        }
        case FilterKind::Invalid:
            return {};
        }
        if (!ok) {
            return {};
        }
        current_valid_global =
            advance_valid_filter_bounds(current_valid_global, filter, clamped_work_bounds);
        current = destination->color;
        destination = (destination == primary) ? secondary : primary;
    }

    result.output = texture_region(current, clamped_work_bounds,
                                   LocalFbRect{0, 0, clamped_work_bounds.w, clamped_work_bounds.h},
                                   clamped_work_bounds.w, clamped_work_bounds.h);
    result.output_bounds = render_bounds_from_framebuffer(clamped_work_bounds, ctx.surface);
    result.valid_output_bounds = render_bounds_from_framebuffer(current_valid_global, ctx.surface);
    return result;
}

} // namespace rmlui_bgfx
