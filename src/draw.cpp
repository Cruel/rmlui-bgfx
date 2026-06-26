#include "rmlui_bgfx_draw.hpp"

#include <algorithm>

namespace rmlui_bgfx {

namespace {

constexpr uint32_t kGradientStopLimit = 16;

[[nodiscard]] float gradient_kind_code(GradientKind kind)
{
    switch (kind) {
    case GradientKind::Linear:
        return 1.0f;
    case GradientKind::RepeatingLinear:
        return 2.0f;
    case GradientKind::Radial:
        return 3.0f;
    case GradientKind::RepeatingRadial:
        return 4.0f;
    case GradientKind::Conic:
        return 5.0f;
    case GradientKind::RepeatingConic:
        return 6.0f;
    case GradientKind::Invalid:
        return 0.0f;
    }
    return 0.0f;
}

[[nodiscard]] const float* draw_transform(bool transform_valid, const float* transform,
                                          const BgfxDrawResources& resources)
{
    return transform_valid && transform ? transform : resources.identity_transform;
}

[[nodiscard]] std::array<float, 4> postprocess_uv_bounds(LocalFbRect source_rect, int texture_width,
                                                         int texture_height)
{
    std::array<float, 4> bounds{};
    if (texture_width > 0 && texture_height > 0 && !is_empty(source_rect)) {
        bounds = uv_rect_for_source_region(source_rect, texture_width, texture_height);
    } else {
        bounds = {0.0f, 0.0f, 1.0f, 1.0f};
    }
    if (bgfx::getCaps() && bgfx::getCaps()->originBottomLeft) {
        const float top = bounds[1];
        const float bottom = bounds[3];
        bounds[1] = 1.0f - bottom;
        bounds[3] = 1.0f - top;
    }
    return bounds;
}

} // namespace

bool BgfxDrawContext::submit_geometry(const RmlUiPass& pass, const BgfxDrawResources& resources,
                                      const GeometryRecord& geometry, const LayerRecord& layer,
                                      const BgfxGeometryDrawState& state) const
{
    if (!bgfx::isValid(resources.rmlui_program) || !bgfx::isValid(geometry.vb) ||
        !bgfx::isValid(geometry.ib) || geometry.index_count == 0) {
        return false;
    }

    if (state.scissor.enabled) {
        const Rml::Rectanglei scissor =
            clamp_scissor_local(state.scissor.region, layer.bounds.framebuffer);
        if (scissor.Width() <= 0 || scissor.Height() <= 0) {
            return false;
        }
        bgfx::setScissor(uint16_t(scissor.Left()), uint16_t(scissor.Top()),
                         uint16_t(scissor.Width()), uint16_t(scissor.Height()));
    }

    bgfx::setVertexBuffer(0, geometry.vb);
    bgfx::setIndexBuffer(geometry.ib, 0, geometry.index_count);
    bgfx::setUniform(resources.projection_uniform, layer.projection);
    bgfx::setUniform(resources.transform_uniform,
                     draw_transform(state.transform_valid, state.transform, resources));
    const float translate[4] = {state.translation.x, state.translation.y, 0.0f, 0.0f};
    bgfx::setUniform(resources.translate_uniform, translate);
    bgfx::setTexture(0, resources.sampler, state.texture);
    bgfx::setState(resources.blend_state | (state.msaa_enabled ? BGFX_STATE_MSAA : 0));
    if (state.clip_mask_enabled) {
        bgfx::setStencil(state.stencil_state, state.stencil_state);
    }
    bgfx::submit(pass.view, resources.rmlui_program);
    return true;
}

bool BgfxDrawContext::submit_gradient(const RmlUiPass& pass, const BgfxDrawResources& resources,
                                      const ShaderRecord& shader, const GeometryRecord& geometry,
                                      const LayerRecord& layer,
                                      const BgfxGradientDrawState& state) const
{
    if (!bgfx::isValid(resources.gradient_program) || !bgfx::isValid(geometry.vb) ||
        !bgfx::isValid(geometry.ib) || geometry.index_count == 0 ||
        shader.kind != ShaderRecordKind::Gradient ||
        shader.gradient.kind == GradientKind::Invalid || shader.gradient.stop_count == 0) {
        return false;
    }

    if (state.scissor.enabled) {
        const Rml::Rectanglei scissor =
            clamp_scissor_local(state.scissor.region, layer.bounds.framebuffer);
        if (scissor.Width() <= 0 || scissor.Height() <= 0) {
            return false;
        }
        bgfx::setScissor(uint16_t(scissor.Left()), uint16_t(scissor.Top()),
                         uint16_t(scissor.Width()), uint16_t(scissor.Height()));
    }

    std::array<float, 8> gradient_params{gradient_kind_code(shader.gradient.kind),
                                         float(shader.gradient.stop_count),
                                         shader.gradient.p_v[0],
                                         shader.gradient.p_v[1],
                                         shader.gradient.p_v[2],
                                         shader.gradient.p_v[3],
                                         0.0f,
                                         0.0f};
    std::array<std::array<float, 4>, kGradientStopLimit> stop_colors{};
    std::array<std::array<float, 4>, 4> stop_positions{};
    for (uint32_t i = 0; i < shader.gradient.stop_count && i < kGradientStopLimit; ++i) {
        stop_colors[i] = shader.gradient.stops[i].color;
        stop_positions[i / 4][i % 4] = shader.gradient.stops[i].position;
    }

    bgfx::setVertexBuffer(0, geometry.vb);
    bgfx::setIndexBuffer(geometry.ib, 0, geometry.index_count);
    bgfx::setUniform(resources.projection_uniform, layer.projection);
    bgfx::setUniform(resources.transform_uniform,
                     draw_transform(state.transform_valid, state.transform, resources));
    const float translate[4] = {state.translation.x, state.translation.y, 0.0f, 0.0f};
    bgfx::setUniform(resources.translate_uniform, translate);
    bgfx::setUniform(resources.gradient_params_uniform, gradient_params.data(), 2);
    bgfx::setUniform(resources.gradient_stops_uniform, stop_colors.data(), kGradientStopLimit);
    bgfx::setUniform(resources.gradient_stop_meta_uniform, stop_positions.data(), 4);
    bgfx::setState(resources.blend_state | (state.msaa_enabled ? BGFX_STATE_MSAA : 0));
    if (state.clip_mask_enabled) {
        bgfx::setStencil(state.stencil_state, state.stencil_state);
    }
    bgfx::submit(pass.view, resources.gradient_program);
    return true;
}

bool BgfxDrawContext::submit_composite(const RmlUiPass& pass, const BgfxDrawResources& resources,
                                       const CompositeOp& op, LocalFbRect source_rect,
                                       LocalFbRect destination_rect, uint32_t stencil_state) const
{
    const bgfx::ProgramHandle program =
        op.filter.enabled ? resources.composite_filter_program : resources.composite_program;
    if (!bgfx::isValid(program) || !bgfx::isValid(resources.fullscreen_vb) ||
        !bgfx::isValid(op.source.texture)) {
        return false;
    }

    if (op.scissor.enabled) {
        const FbRect target_scissor{op.scissor.region.Left(), op.scissor.region.Top(),
                                    op.scissor.region.Width(), op.scissor.region.Height()};
        const FbRect clipped_scissor = intersect(target_scissor, destination_rect);
        if (is_empty(clipped_scissor)) {
            return true;
        }
        bgfx::setScissor(uint16_t(clipped_scissor.x), uint16_t(clipped_scissor.y),
                         uint16_t(clipped_scissor.w), uint16_t(clipped_scissor.h));
    }

    bgfx::setVertexBuffer(0, resources.fullscreen_vb);
    bgfx::setTexture(0, resources.sampler, op.source.texture);
    const std::array<float, 4> bounds =
        postprocess_uv_bounds(source_rect, op.source.texture_width, op.source.texture_height);
    bgfx::setUniform(resources.texcoord_bounds_uniform, bounds.data());
    if (op.filter.enabled) {
        const float opacity[4] = {op.filter.opacity, 0.0f, 0.0f, 0.0f};
        bgfx::setUniform(resources.opacity_uniform, opacity);
        bgfx::setUniform(resources.color_matrix_uniform, op.filter.color_matrix.data());
    }
    const uint64_t state = op.blend_mode == Rml::BlendMode::Replace
                               ? (BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A)
                               : resources.blend_state;
    bgfx::setState(state | (op.msaa_enabled ? BGFX_STATE_MSAA : 0));
    if (op.apply_destination_stencil) {
        bgfx::setStencil(stencil_state, stencil_state);
    }
    bgfx::submit(pass.view, program);
    return true;
}

bool BgfxDrawContext::submit_copy(const RmlUiPass& pass, const BgfxDrawResources& resources,
                                  bgfx::TextureHandle source, const Rml::Rectanglei& source_region,
                                  int source_width, int source_height, bool flip_y) const
{
    if (!bgfx::isValid(resources.copy_program) || !bgfx::isValid(resources.fullscreen_vb) ||
        !bgfx::isValid(source)) {
        return false;
    }
    const float left = float(source_region.Left()) / float(std::max(source_width, 1));
    const float top = float(source_region.Top()) / float(std::max(source_height, 1));
    const float right = float(source_region.Right()) / float(std::max(source_width, 1));
    const float bottom = float(source_region.Bottom()) / float(std::max(source_height, 1));
    const float bounds[4] = {left, flip_y ? bottom : top, right, flip_y ? top : bottom};
    bgfx::setVertexBuffer(0, resources.fullscreen_vb);
    bgfx::setTexture(0, resources.sampler, source);
    bgfx::setUniform(resources.texcoord_bounds_uniform, bounds);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
    bgfx::submit(pass.view, resources.copy_program);
    return true;
}

void BgfxDrawContext::submit_blit(const RmlUiPass& pass, bgfx::TextureHandle destination,
                                  bgfx::TextureHandle source,
                                  const Rml::Rectanglei& source_region) const
{
    bgfx::blit(pass.view, destination, 0, 0, source, uint16_t(source_region.Left()),
               uint16_t(source_region.Top()), uint16_t(source_region.Width()),
               uint16_t(source_region.Height()));
}

bool BgfxDrawContext::submit_fullscreen_postprocess(
    const RmlUiPass& pass, const BgfxDrawResources& resources, bgfx::TextureHandle source,
    bgfx::ProgramHandle program, const std::function<void()>& bind_uniforms) const
{
    if (!bgfx::isValid(resources.fullscreen_vb) || !bgfx::isValid(source) ||
        !bgfx::isValid(program)) {
        return false;
    }
    bgfx::setVertexBuffer(0, resources.fullscreen_vb);
    bgfx::setTexture(0, resources.sampler, source);
    bind_uniforms();
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
    bgfx::submit(pass.view, program);
    return true;
}

bool BgfxDrawContext::submit_opacity(const RmlUiPass& pass, const BgfxDrawResources& resources,
                                     bgfx::TextureHandle source, const float* opacity) const
{
    return submit_fullscreen_postprocess(pass, resources, source, resources.opacity_program, [&]() {
        bgfx::setUniform(resources.opacity_uniform, opacity);
    });
}

bool BgfxDrawContext::submit_color_matrix(const RmlUiPass& pass, const BgfxDrawResources& resources,
                                          bgfx::TextureHandle source, const float* matrix) const
{
    return submit_fullscreen_postprocess(
        pass, resources, source, resources.color_matrix_program,
        [&]() { bgfx::setUniform(resources.color_matrix_uniform, matrix); });
}

bool BgfxDrawContext::submit_blur(const RmlUiPass& pass, const BgfxDrawResources& resources,
                                  bgfx::TextureHandle source, const float* params,
                                  const float* weights, const float* texcoord_bounds) const
{
    return submit_fullscreen_postprocess(pass, resources, source, resources.blur_program, [&]() {
        bgfx::setUniform(resources.blur_params_uniform, params);
        bgfx::setUniform(resources.blur_weights_uniform, weights);
        bgfx::setUniform(resources.texcoord_bounds_uniform, texcoord_bounds);
    });
}

bool BgfxDrawContext::submit_drop_shadow(const RmlUiPass& pass, const BgfxDrawResources& resources,
                                         bgfx::TextureHandle source, const float* color,
                                         const float* offset) const
{
    return submit_fullscreen_postprocess(
        pass, resources, source, resources.drop_shadow_program, [&]() {
            bgfx::setUniform(resources.shadow_color_uniform, color);
            bgfx::setUniform(resources.shadow_offset_uniform, offset);
        });
}

bool BgfxDrawContext::submit_mask_image(const RmlUiPass& pass, const BgfxDrawResources& resources,
                                        bgfx::TextureHandle source, bgfx::TextureHandle mask,
                                        const std::array<float, 4>& mask_transform,
                                        const std::array<float, 4>& source_bounds) const
{
    if (!bgfx::isValid(resources.fullscreen_vb) ||
        !bgfx::isValid(resources.mask_multiply_program) || !bgfx::isValid(source) ||
        !bgfx::isValid(mask)) {
        return false;
    }
    bgfx::setVertexBuffer(0, resources.fullscreen_vb);
    bgfx::setTexture(0, resources.sampler, source);
    bgfx::setTexture(1, resources.mask_sampler, mask);
    bgfx::setUniform(resources.texcoord_bounds_uniform, source_bounds.data());
    bgfx::setUniform(resources.mask_texcoord_transform_uniform, mask_transform.data());
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
    bgfx::submit(pass.view, resources.mask_multiply_program);
    return true;
}

bool BgfxDrawContext::submit_stencil_decrement(const RmlUiPass& pass,
                                               const BgfxDrawResources& resources,
                                               uint32_t stencil_state) const
{
    if (!bgfx::isValid(resources.fullscreen_vb) || !bgfx::isValid(resources.white_texture) ||
        !bgfx::isValid(resources.composite_program)) {
        return false;
    }
    bgfx::setVertexBuffer(0, resources.fullscreen_vb);
    bgfx::setTexture(0, resources.sampler, resources.white_texture);
    const float bounds[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    bgfx::setUniform(resources.texcoord_bounds_uniform, bounds);
    bgfx::setState(BGFX_STATE_NONE);
    bgfx::setStencil(stencil_state, stencil_state);
    bgfx::submit(pass.view, resources.composite_program);
    return true;
}

bool BgfxDrawContext::submit_clip_mask(const RmlUiPass& pass, const BgfxDrawResources& resources,
                                       const GeometryRecord& geometry, const LayerRecord& layer,
                                       const BgfxClipMaskDrawState& state) const
{
    if (!bgfx::isValid(resources.rmlui_program) || !bgfx::isValid(geometry.vb) ||
        !bgfx::isValid(geometry.ib) || geometry.index_count == 0) {
        return false;
    }

    if (state.scissor.enabled) {
        const Rml::Rectanglei clipped =
            clamp_scissor_local(state.scissor.region, layer.bounds.framebuffer);
        if (clipped.Width() <= 0 || clipped.Height() <= 0) {
            return false;
        }
        bgfx::setScissor(uint16_t(clipped.Left()), uint16_t(clipped.Top()),
                         uint16_t(clipped.Width()), uint16_t(clipped.Height()));
    }

    bgfx::setVertexBuffer(0, geometry.vb);
    bgfx::setIndexBuffer(geometry.ib, 0, geometry.index_count);
    bgfx::setUniform(resources.projection_uniform, layer.projection);
    bgfx::setUniform(resources.transform_uniform,
                     draw_transform(state.transform_valid, state.transform, resources));
    const float translate[4] = {state.translation.x, state.translation.y, 0.0f, 0.0f};
    bgfx::setUniform(resources.translate_uniform, translate);
    bgfx::setTexture(0, resources.sampler, resources.white_texture);
    bgfx::setState(state.msaa_enabled ? BGFX_STATE_MSAA : BGFX_STATE_NONE);
    bgfx::setStencil(state.stencil_state, state.stencil_state);
    bgfx::submit(pass.view, resources.rmlui_program);
    return true;
}

} // namespace rmlui_bgfx
