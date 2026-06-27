#include "rmlui_bgfx_filters.hpp"
#include "rmlui_bgfx_filter_paths.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace rmlui_bgfx {

namespace {

[[nodiscard]] TextureRegion texture_region(bgfx::TextureHandle texture, GlobalFbRect global_bounds,
                                           LocalFbRect local_rect, int texture_width,
                                           int texture_height)
{
    return TextureRegion{texture, global_bounds, local_rect, texture_width, texture_height};
}

[[nodiscard]] const char* filter_kind_name(FilterKind kind)
{
    switch (kind) {
    case FilterKind::Blur:
        return "blur";
    case FilterKind::DropShadow:
        return "drop-shadow";
    case FilterKind::Opacity:
        return "opacity";
    case FilterKind::ColorMatrix:
        return "color-matrix";
    case FilterKind::MaskImage:
        return "mask-image";
    case FilterKind::Invalid:
        return "invalid";
    }
    return "unknown";
}

void trace_rect(const char* label, FbRect rect)
{
    std::fprintf(stderr, " %s=(%d,%d %dx%d)", label, rect.x, rect.y, rect.w, rect.h);
}

void trace_texture(const char* label, const TextureRegion& region)
{
    std::fprintf(stderr, " %s_tex=%u %s_size=%dx%d", label,
                 bgfx::isValid(region.texture) ? region.texture.idx : 65535u, label,
                 region.texture_width, region.texture_height);
    trace_rect("global", region.global_bounds);
    trace_rect("local", region.local_rect);
}

void trace_filter_chain(const BgfxFilterPipelineContext& ctx,
                        const std::vector<FilterRecord>& filter_chain)
{
    if (!ctx.trace_filter_pipeline) {
        return;
    }
    std::fprintf(stderr, "[rmlui-bgfx][filter] chain count=%zu", filter_chain.size());
    for (const FilterRecord& filter : filter_chain) {
        std::fprintf(stderr, " %s", filter_kind_name(filter.kind));
        if (filter.kind == FilterKind::Blur || filter.kind == FilterKind::DropShadow) {
            std::fprintf(stderr, " sigma=%.3f", filter.sigma);
        }
        if (filter.kind == FilterKind::DropShadow) {
            std::fprintf(stderr, " offset=(%.3f,%.3f)", filter.offset[0], filter.offset[1]);
        }
    }
    std::fprintf(stderr, "\n");
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

[[nodiscard]] bool filter_chain_has_drop_shadow(const std::vector<FilterRecord>& filters)
{
    return std::any_of(filters.begin(), filters.end(), [](const FilterRecord& filter) {
        return filter.kind == FilterKind::DropShadow;
    });
}

[[nodiscard]] RenderTargetRecord* target_for_texture(bgfx::TextureHandle texture,
                                                     std::array<RenderTargetRecord*, 3> targets)
{
    if (!bgfx::isValid(texture)) {
        return nullptr;
    }
    for (RenderTargetRecord* target : targets) {
        if (target && bgfx::isValid(target->color) && target->color.idx == texture.idx) {
            return target;
        }
    }
    return nullptr;
}

[[nodiscard]] bgfx::TextureHandle resolve_mask_texture(const BgfxFilterPipelineContext& ctx,
                                                       const FilterRecord& filter)
{
    if (filter.resource != 0) {
        auto tex_it = ctx.textures.find(Rml::TextureHandle(filter.resource));
        if (tex_it != ctx.textures.end()) {
            return tex_it->second.handle;
        }
        return BGFX_INVALID_HANDLE;
    }

    const FbRect mask_bounds = clamp_to_surface(
        align_outward_for_render_target(FbRect{filter.mask_bounds[0], filter.mask_bounds[1],
                                               filter.mask_bounds[2], filter.mask_bounds[3]}),
        ctx.surface);
    if (is_empty(mask_bounds)) {
        return BGFX_INVALID_HANDLE;
    }
    RenderTargetRecord* blend_mask = ctx.target_cache.acquire_postprocess_target(
        PostprocessTargetKind::BlendMask, mask_bounds, ctx.surface);
    if (blend_mask) {
        return blend_mask->color;
    }
    return BGFX_INVALID_HANDLE;
}

[[nodiscard]] std::array<float, 4> mask_uv_transform(FbRect destination_bounds, FbRect mask_bounds)
{
    auto transform = compute_mask_uv_transform(destination_bounds, mask_bounds);
    if (bgfx::getCaps() && bgfx::getCaps()->originBottomLeft) {
        const float inv_mask_h = 1.0f / float(std::max(mask_bounds.h, 1));
        transform[1] = float(destination_bounds.h) * inv_mask_h;
        const float destination_bottom = float(destination_bounds.y + destination_bounds.h);
        transform[3] = 1.0f - (destination_bottom - float(mask_bounds.y)) * inv_mask_h;
    }
    return transform;
}

struct BlurShaderParameters {
    float texel_scale = 1.0f;
    float sigma = 0.0f;
    float weights[4] = {1.0f, 0.0f, 0.0f, 0.0f};
};

[[nodiscard]] BlurShaderParameters blur_shader_parameters(float desired_sigma)
{
    BlurShaderParameters params;
    if (desired_sigma < 0.5f) {
        return params;
    }

    // The GL3 backend avoids sparse large-radius sampling by downscaling before its
    // fixed 7-tap blur and then upscaling afterwards. This bgfx path keeps the current
    // two-pass target structure for now, so use a denser shader kernel instead of the
    // old 7-tap, large-stride approximation that produced visible blur bands.
    constexpr float samples_per_sigma = 12.0f;
    params.texel_scale = std::max(1.0f, desired_sigma / samples_per_sigma);
    params.sigma = desired_sigma;
    return params;
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
    if (ctx.trace_filter_pipeline) {
        std::fprintf(stderr,
                     "[rmlui-bgfx][filter] composite name=%s source_tex=%u destination_fb=%u",
                     op.name ? op.name : "<null>",
                     bgfx::isValid(op.source.texture) ? op.source.texture.idx : 65535u,
                     bgfx::isValid(op.destination) ? op.destination.idx : 65535u);
        trace_rect("source_local", op.source.local_rect);
        trace_rect("destination", op.destination_rect);
        std::fprintf(stderr, "\n");
    }
    if (!ctx.ensure_fullscreen_geometry || !ctx.ensure_fullscreen_geometry() ||
        !bgfx::isValid(op.source.texture)) {
        if (ctx.trace_filter_pipeline) {
            std::fprintf(
                stderr,
                "[rmlui-bgfx][filter] composite reject resources ensure=%d source_valid=%d\n",
                ctx.ensure_fullscreen_geometry ? 1 : 0, bgfx::isValid(op.source.texture) ? 1 : 0);
        }
        return false;
    }

    if (bgfx::isValid(op.destination) &&
        texture_attached_to_framebuffer(ctx, op.source.texture, op.destination)) {
        if (ctx.trace_filter_pipeline) {
            std::fprintf(
                stderr,
                "[rmlui-bgfx][filter] composite reject feedback source_tex=%u destination_fb=%u\n",
                op.source.texture.idx, op.destination.idx);
        }
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
        if (ctx.trace_filter_pipeline) {
            std::fprintf(stderr, "[rmlui-bgfx][filter] composite reject no-pass error=%s\n",
                         ctx.pass_builder.error() ? ctx.pass_builder.error() : "<none>");
        }
        return false;
    }
    ctx.perf.add_composite(area(destination_rect), is_full_frame);
    const bool submitted =
        ctx.draw_context.submit_composite(*pass, ctx.resources, op, source_rect, destination_rect,
                                          stencil_test_state_for_ref(op.stencil_ref));
    if (ctx.trace_filter_pipeline) {
        std::fprintf(stderr, "[rmlui-bgfx][filter] composite submitted=%d view=%u\n",
                     submitted ? 1 : 0, unsigned(pass->view));
    }
    return submitted;
}

FilterApplyResult
BgfxFilterPipeline::apply(const BgfxFilterPipelineContext& ctx, TextureRegion source,
                          const RenderBounds& source_bounds,
                          Rml::Span<const Rml::CompiledFilterHandle> filter_handles) const
{
    switch (ctx.render_path) {
    case RenderPath::Reference:
        if (ctx.fail_frame) {
            ctx.fail_frame("reference render path must use BgfxReferenceRenderer filter pipeline");
        }
        return {};
    case RenderPath::Optimized:
        return apply_filters_optimized(*this, ctx, source, source_bounds, filter_handles);
    }
    return {};
}

FilterApplyResult
BgfxFilterPipeline::apply_common(const BgfxFilterPipelineContext& ctx, TextureRegion source,
                                 const RenderBounds& source_bounds,
                                 Rml::Span<const Rml::CompiledFilterHandle> filter_handles) const
{
    FilterApplyResult result;
    const GlobalFbRect source_valid_global_bounds =
        intersect(source.global_bounds, source_bounds.framebuffer);
    if (is_empty(source_valid_global_bounds) || !bgfx::isValid(source.texture)) {
        return {};
    }

    source.local_rect = {
        source.local_rect.x + source_valid_global_bounds.x - source.global_bounds.x,
        source.local_rect.y + source_valid_global_bounds.y - source.global_bounds.y,
        source_valid_global_bounds.w, source_valid_global_bounds.h};
    source.global_bounds = source_valid_global_bounds;
    result.output = source;
    result.output_bounds = render_bounds_from_framebuffer(source_valid_global_bounds, ctx.surface);
    result.valid_output_bounds = result.output_bounds;
    if (ctx.trace_filter_pipeline) {
        std::fprintf(stderr, "[rmlui-bgfx][filter] apply handles=%zu", filter_handles.size());
        trace_texture("source", source);
        trace_rect("allocation", source_bounds.framebuffer);
        std::fprintf(stderr, "\n");
    }
    if (filter_handles.empty()) {
        return result;
    }

    std::vector<FilterRecord> filter_chain = resolve(ctx, filter_handles);
    if (filter_chain.empty()) {
        return result;
    }
    trace_filter_chain(ctx, filter_chain);

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
    FbRect clamped_work_bounds =
        clamp_to_surface(align_outward_for_render_target(expanded), ctx.surface);
    if (ctx.clamp_work_bounds_to_source) {
        clamped_work_bounds = intersect(clamped_work_bounds, source_bounds.framebuffer);
    }
    if (ctx.trace_filter_pipeline) {
        trace_rect("expanded", expanded);
        trace_rect("work", clamped_work_bounds);
        std::fprintf(stderr, "\n");
    }
    if (is_empty(clamped_work_bounds)) {
        return {};
    }
    RenderTargetRecord* primary = ctx.target_cache.acquire_postprocess_target(
        PostprocessTargetKind::Primary, clamped_work_bounds, ctx.surface);
    RenderTargetRecord* secondary = ctx.target_cache.acquire_postprocess_target(
        PostprocessTargetKind::Secondary, clamped_work_bounds, ctx.surface);
    const bool needs_tertiary = filter_chain_has_drop_shadow(filter_chain);
    RenderTargetRecord* tertiary =
        needs_tertiary ? ctx.target_cache.acquire_postprocess_target(
                             PostprocessTargetKind::Tertiary, clamped_work_bounds, ctx.surface)
                       : nullptr;
    if (!primary || !secondary || (needs_tertiary && !tertiary)) {
        return {};
    }
    if (ctx.trace_filter_pipeline) {
        std::fprintf(stderr,
                     "[rmlui-bgfx][filter] targets primary_tex=%u primary_fb=%u primary_size=%dx%d",
                     bgfx::isValid(primary->color) ? primary->color.idx : 65535u,
                     bgfx::isValid(primary->framebuffer) ? primary->framebuffer.idx : 65535u,
                     primary->texture_width, primary->texture_height);
        trace_rect("primary_bounds", primary->bounds);
        std::fprintf(stderr, " secondary_tex=%u secondary_fb=%u secondary_size=%dx%d",
                     bgfx::isValid(secondary->color) ? secondary->color.idx : 65535u,
                     bgfx::isValid(secondary->framebuffer) ? secondary->framebuffer.idx : 65535u,
                     secondary->texture_width, secondary->texture_height);
        trace_rect("secondary_bounds", secondary->bounds);
        std::fprintf(stderr, "\n");
    }

    if (filter_chain.size() == 1 && filter_chain[0].kind == FilterKind::MaskImage) {
        const bgfx::TextureHandle mask_texture = resolve_mask_texture(ctx, filter_chain[0]);
        if (!bgfx::isValid(mask_texture)) {
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
        if (texture_attached_to_framebuffer(ctx, mask_texture, destination->framebuffer)) {
            return {};
        }
        const FbRect mask_bounds{filter_chain[0].mask_bounds[0], filter_chain[0].mask_bounds[1],
                                 filter_chain[0].mask_bounds[2], filter_chain[0].mask_bounds[3]};
        const auto mask_transform = mask_uv_transform(destination->bounds, mask_bounds);
        const FbRect source_sample_local{
            source.local_rect.x + destination->bounds.x - source.global_bounds.x,
            source.local_rect.y + destination->bounds.y - source.global_bounds.y,
            destination->bounds.w,
            destination->bounds.h,
        };
        const auto source_uv = uv_rect_for_source_region(source_sample_local, source.texture_width,
                                                         source.texture_height);
        if (!ctx.draw_context.submit_mask_image(*pass, ctx.resources, source.texture, mask_texture,
                                                mask_transform, source_uv)) {
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

    const FbRect source_copy_global = intersect(source.global_bounds, clamped_work_bounds);
    if (is_empty(source_copy_global)) {
        return {};
    }
    const FbRect source_copy_local{
        source.local_rect.x + source_copy_global.x - source.global_bounds.x,
        source.local_rect.y + source_copy_global.y - source.global_bounds.y, source_copy_global.w,
        source_copy_global.h};
    const FbRect copy_destination{source_copy_global.x - clamped_work_bounds.x,
                                  source_copy_global.y - clamped_work_bounds.y,
                                  source_copy_global.w, source_copy_global.h};
    if (ctx.trace_filter_pipeline) {
        std::fprintf(stderr, "[rmlui-bgfx][filter] copy");
        trace_rect("source_global", source_copy_global);
        trace_rect("source_local", source_copy_local);
        trace_rect("destination", copy_destination);
        std::fprintf(stderr, "\n");
    }
    if (auto clear_pass = ctx.pass_builder.layer_clear(primary->framebuffer, primary->texture_width,
                                                       primary->texture_height)) {
        bgfx::touch(clear_pass->view);
    } else {
        return {};
    }
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
            const bgfx::TextureHandle mask_texture = resolve_mask_texture(ctx, filter);
            if (!bgfx::isValid(mask_texture) || !ctx.ensure_fullscreen_geometry ||
                !ctx.ensure_fullscreen_geometry() ||
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
            if (texture_attached_to_framebuffer(ctx, mask_texture, destination->framebuffer)) {
                if (ctx.fail_frame) {
                    ctx.fail_frame("mask-image filter feedback loop");
                }
                return {};
            }
            const FbRect mask_bounds{filter.mask_bounds[0], filter.mask_bounds[1],
                                     filter.mask_bounds[2], filter.mask_bounds[3]};
            const auto mask_transform = mask_uv_transform(destination->bounds, mask_bounds);
            ok = ctx.draw_context.submit_mask_image(*pass, ctx.resources, current, mask_texture,
                                                    mask_transform);
            current_valid_rect = {0, 0, destination->texture_width, destination->texture_height};
            break;
        }
        case FilterKind::Blur: {
            const BlurShaderParameters blur = blur_shader_parameters(filter.sigma);
            const std::array<float, 4> bounds{0.0f, 0.0f, 1.0f, 1.0f};
            if (ctx.trace_filter_pipeline) {
                std::fprintf(
                    stderr,
                    "[rmlui-bgfx][filter] blur sigma=%.3f shader_sigma=%.3f texel_scale=%.3f",
                    filter.sigma, blur.sigma, blur.texel_scale);
                trace_rect("current_valid_rect", current_valid_rect);
                std::fprintf(
                    stderr,
                    " current_tex=%u destination_tex=%u bounds=(%.6f,%.6f %.6f,%.6f) mode=%s\n",
                    bgfx::isValid(current) ? current.idx : 65535u,
                    bgfx::isValid(destination->color) ? destination->color.idx : 65535u, bounds[0],
                    bounds[1], bounds[2], bounds[3], "initialized-full-texture");
            }
            float params[4] = {0.0f,
                               blur.texel_scale / float(std::max(destination->texture_height, 1)),
                               blur.sigma, blur.texel_scale};
            if (!fullscreen_filter_pass(
                    ctx, current, *destination, "RmlUi.FilterBlurV",
                    [&](const RmlUiPass& pass) {
                        return ctx.draw_context.submit_blur(pass, ctx.resources, current, params,
                                                            blur.weights, bounds.data());
                    },
                    RmlUiPassReason::FilterBlur)) {
                return {};
            }
            ctx.perf.add_blur();
            current = destination->color;
            destination = (destination == primary) ? secondary : primary;
            params[0] = blur.texel_scale / float(std::max(destination->texture_width, 1));
            params[1] = 0.0f;
            if (!fullscreen_filter_pass(
                    ctx, current, *destination, "RmlUi.FilterBlurH",
                    [&](const RmlUiPass& pass) {
                        return ctx.draw_context.submit_blur(pass, ctx.resources, current, params,
                                                            blur.weights, bounds.data());
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
            if (!tertiary) {
                return {};
            }
            RenderTargetRecord* original = target_for_texture(
                current, std::array<RenderTargetRecord*, 3>{primary, secondary, tertiary});
            if (!original) {
                return {};
            }
            std::array<RenderTargetRecord*, 2> scratch{};
            size_t scratch_count = 0;
            for (RenderTargetRecord* target : {primary, secondary, tertiary}) {
                if (target != original) {
                    scratch[scratch_count++] = target;
                }
            }
            if (scratch_count != scratch.size()) {
                return {};
            }
            RenderTargetRecord* shadow = scratch[0];
            RenderTargetRecord* final = scratch[1];

            const float color[4] = {filter.color[0], filter.color[1], filter.color[2],
                                    filter.color[3]};
            const float offset[4] = {filter.offset[0] / float(std::max(shadow->texture_width, 1)),
                                     -filter.offset[1] / float(std::max(shadow->texture_height, 1)),
                                     0.0f, 0.0f};
            if (!fullscreen_filter_pass(
                    ctx, original->color, *shadow, "RmlUi.FilterDropShadowExtract",
                    [&](const RmlUiPass& pass) {
                        return ctx.draw_context.submit_drop_shadow(pass, ctx.resources,
                                                                   original->color, color, offset);
                    },
                    RmlUiPassReason::FilterDropShadow)) {
                return {};
            }
            ctx.perf.add_dropshadow();
            if (filter.sigma >= 0.5f) {
                const BlurShaderParameters blur = blur_shader_parameters(filter.sigma);
                const std::array<float, 4> bounds{0.0f, 0.0f, 1.0f, 1.0f};
                float params[4] = {0.0f,
                                   blur.texel_scale / float(std::max(final->texture_height, 1)),
                                   blur.sigma, blur.texel_scale};
                if (!fullscreen_filter_pass(
                        ctx, shadow->color, *final, "RmlUi.FilterDropShadowBlurV",
                        [&](const RmlUiPass& pass) {
                            return ctx.draw_context.submit_blur(pass, ctx.resources, shadow->color,
                                                                params, blur.weights,
                                                                bounds.data());
                        },
                        RmlUiPassReason::FilterBlur)) {
                    return {};
                }
                ctx.perf.add_blur();
                std::swap(shadow, final);
                params[0] = blur.texel_scale / float(std::max(final->texture_width, 1));
                params[1] = 0.0f;
                if (!fullscreen_filter_pass(
                        ctx, shadow->color, *final, "RmlUi.FilterDropShadowBlurH",
                        [&](const RmlUiPass& pass) {
                            return ctx.draw_context.submit_blur(pass, ctx.resources, shadow->color,
                                                                params, blur.weights,
                                                                bounds.data());
                        },
                        RmlUiPassReason::FilterBlur)) {
                    return {};
                }
                ctx.perf.add_blur();
                std::swap(shadow, final);
            }
            if (!composite(ctx,
                           make_composite_op(
                               texture_region(
                                   shadow->color, shadow->bounds,
                                   LocalFbRect{0, 0, shadow->texture_width, shadow->texture_height},
                                   shadow->texture_width, shadow->texture_height),
                               final->framebuffer, Rml::BlendMode::Replace, ScissorState{false, {}},
                               false, 1, RmlUiPassKind::Postprocess,
                               RmlUiPassReason::FilterDropShadowComposite,
                               "RmlUi.FilterDropShadowCopy",
                               LocalFbRect{0, 0, final->texture_width, final->texture_height}))) {
                return {};
            }
            if (!composite(ctx,
                           make_composite_op(
                               texture_region(original->color, original->bounds,
                                              LocalFbRect{0, 0, original->texture_width,
                                                          original->texture_height},
                                              original->texture_width, original->texture_height),
                               final->framebuffer, Rml::BlendMode::Blend, ScissorState{false, {}},
                               false, 1, RmlUiPassKind::Postprocess,
                               RmlUiPassReason::FilterDropShadowComposite,
                               "RmlUi.FilterDropShadowComposite",
                               LocalFbRect{0, 0, final->texture_width, final->texture_height}))) {
                return {};
            }
            destination = final;
            ok = true;
            current_valid_rect = {0, 0, final->texture_width, final->texture_height};
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
    if (ctx.trace_filter_pipeline) {
        std::fprintf(stderr, "[rmlui-bgfx][filter] result");
        trace_texture("output", result.output);
        trace_rect("valid", result.valid_output_bounds.framebuffer);
        std::fprintf(stderr, "\n");
    }
    return result;
}

} // namespace rmlui_bgfx
