#pragma once

#include "rmlui_bgfx_types.hpp"

#include <bgfx/bgfx.h>

#include <RmlUi/Core/Types.h>

#include <array>
#include <cstdint>
#include <functional>

namespace rmlui_bgfx {

struct BgfxDrawResources {
    bgfx::VertexBufferHandle fullscreen_vb = BGFX_INVALID_HANDLE;
    const bgfx::VertexLayout* geometry_layout = nullptr;
    bgfx::TextureHandle white_texture = BGFX_INVALID_HANDLE;

    bgfx::UniformHandle sampler = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle mask_sampler = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle projection_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle transform_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle translate_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle gradient_params_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle gradient_stops_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle gradient_stop_meta_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle texcoord_bounds_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle mask_texcoord_transform_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle opacity_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle color_matrix_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle blur_params_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle blur_weights_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle shadow_color_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle shadow_offset_uniform = BGFX_INVALID_HANDLE;

    bgfx::ProgramHandle rmlui_program = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle composite_program = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle composite_filter_program = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle copy_program = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle gradient_program = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle mask_multiply_program = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle opacity_program = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle color_matrix_program = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle blur_program = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle drop_shadow_program = BGFX_INVALID_HANDLE;

    const float* identity_transform = nullptr;
    uint64_t blend_state = 0;
};

struct BgfxGeometryDrawState {
    Rml::Vector2f translation;
    bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
    ScissorState scissor;
    bool transform_valid = false;
    const float* transform = nullptr;
    bool clip_mask_enabled = false;
    bool msaa_enabled = false;
    uint32_t stencil_state = 0;
    bool flip_texture_y = false;
};

struct BgfxGradientDrawState {
    Rml::Vector2f translation;
    ScissorState scissor;
    bool transform_valid = false;
    const float* transform = nullptr;
    bool clip_mask_enabled = false;
    bool msaa_enabled = false;
    uint32_t stencil_state = 0;
};

struct BgfxClipMaskDrawState {
    Rml::Vector2f translation;
    ScissorState scissor;
    bool transform_valid = false;
    const float* transform = nullptr;
    bool msaa_enabled = false;
    uint32_t stencil_state = 0;
};

class BgfxDrawContext {
public:
    bool submit_geometry(const RmlUiPass& pass, const BgfxDrawResources& resources,
                         const GeometryRecord& geometry, const LayerRecord& layer,
                         const BgfxGeometryDrawState& state) const;

    bool submit_gradient(const RmlUiPass& pass, const BgfxDrawResources& resources,
                         const ShaderRecord& shader, const GeometryRecord& geometry,
                         const LayerRecord& layer, const BgfxGradientDrawState& state) const;

    bool submit_composite(const RmlUiPass& pass, const BgfxDrawResources& resources,
                          const CompositeOp& op, LocalFbRect source_rect,
                          LocalFbRect destination_rect, uint32_t stencil_state) const;

    bool submit_copy(const RmlUiPass& pass, const BgfxDrawResources& resources,
                     bgfx::TextureHandle source, const Rml::Rectanglei& source_region,
                     int source_width, int source_height, bool flip_y = false) const;

    void submit_blit(const RmlUiPass& pass, bgfx::TextureHandle destination,
                     bgfx::TextureHandle source, const Rml::Rectanglei& source_region) const;

    bool submit_fullscreen_postprocess(const RmlUiPass& pass, const BgfxDrawResources& resources,
                                       bgfx::TextureHandle source, bgfx::ProgramHandle program,
                                       const std::function<void()>& bind_uniforms) const;

    bool submit_opacity(const RmlUiPass& pass, const BgfxDrawResources& resources,
                        bgfx::TextureHandle source, const float* opacity) const;
    bool submit_color_matrix(const RmlUiPass& pass, const BgfxDrawResources& resources,
                             bgfx::TextureHandle source, const float* matrix) const;
    bool submit_blur(const RmlUiPass& pass, const BgfxDrawResources& resources,
                     bgfx::TextureHandle source, const float* params, const float* weights,
                     const float* texcoord_bounds) const;
    bool submit_drop_shadow(const RmlUiPass& pass, const BgfxDrawResources& resources,
                            bgfx::TextureHandle source, const float* color,
                            const float* offset) const;

    bool submit_mask_image(const RmlUiPass& pass, const BgfxDrawResources& resources,
                           bgfx::TextureHandle source, bgfx::TextureHandle mask,
                           const std::array<float, 4>& mask_transform,
                           const std::array<float, 4>& source_bounds = {0.0f, 0.0f, 1.0f,
                                                                        1.0f}) const;

    bool submit_stencil_decrement(const RmlUiPass& pass, const BgfxDrawResources& resources,
                                  uint32_t stencil_state) const;

    bool submit_clip_mask(const RmlUiPass& pass, const BgfxDrawResources& resources,
                          const GeometryRecord& geometry, const LayerRecord& layer,
                          const BgfxClipMaskDrawState& state) const;
};

} // namespace rmlui_bgfx
