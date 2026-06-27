#include <rmlui_bgfx/render_interface.hpp>

#include "rmlui_bgfx_bounds.hpp"
#include "rmlui_bgfx_draw.hpp"
#include "rmlui_bgfx_filters.hpp"
#include "rmlui_bgfx_layers.hpp"
#include "rmlui_bgfx_passes.hpp"
#include "rmlui_bgfx_planning.hpp"
#include "rmlui_bgfx_reference_renderer.hpp"
#include "rmlui_bgfx_target_cache.hpp"
#include "rmlui_bgfx_types.hpp"

#include <RmlUi/Core/Dictionary.h>
#include <RmlUi/Core/DecorationTypes.h>
#include <RmlUi/Core/Types.h>
#include <RmlUi/Core/Unit.h>
#include <RmlUi/Core/Variant.h>
#include <bx/math.h>
#include <bx/timer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <cstring>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rmlui_bgfx {

namespace {

constexpr uint64_t kRmlTextureFlags = 0;
constexpr uint64_t kRmlBlendState =
    BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
    BGFX_STATE_BLEND_FUNC_SEPARATE(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA,
                                   BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA);
constexpr uint32_t kGradientStopLimit = 16;

struct RmlVertex {
    float px;
    float py;
    uint32_t rgba;
    float u;
    float v;
};

TextureRegion texture_region(bgfx::TextureHandle texture, GlobalFbRect global_bounds,
                             LocalFbRect local_rect, int texture_width, int texture_height)
{
    return TextureRegion{texture, global_bounds, local_rect, texture_width, texture_height};
}

CompositeOp make_composite_op(TextureRegion source, bgfx::FrameBufferHandle destination,
                              Rml::BlendMode blend_mode, ScissorState scissor,
                              bool apply_destination_stencil, uint8_t stencil_ref,
                              RmlUiPassKind kind, RmlUiPassReason reason, const char* name,
                              LocalFbRect destination_rect = {}, CompositeFilterState filter = {})
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

bool is_full_frame_rect(FbRect rect, int width, int height)
{
    return !is_empty(rect) && rect.x == 0 && rect.y == 0 && rect.w >= width && rect.h >= height;
}

RmlUiPassReason copy_pass_reason_from_name(const char* name)
{
    if (name && std::strcmp(name, "RmlUi.SaveLayerAsTexture") == 0) {
        return RmlUiPassReason::SaveTextureCopy;
    }
    if (name && std::strcmp(name, "RmlUi.SaveLayerAsMaskImage") == 0) {
        return RmlUiPassReason::SaveMaskCopy;
    }
    return RmlUiPassReason::OtherCopy;
}

FbRect active_scissor_bounds(const ScissorState& scissor, const FbRect& layer_bounds)
{
    if (!scissor.enabled)
        return {};
    const Rml::Rectanglei local = clamp_scissor_local(scissor.region, layer_bounds);
    return {local.Left(), local.Top(), local.Width(), local.Height()};
}

FbRect clip_work_bounds(const LayerRecord* layer, const ScissorState& scissor)
{
    if (!layer)
        return {};
    const FbRect layer_bounds = layer->bounds.framebuffer;
    if (is_empty(layer_bounds))
        return {};
    const FbRect layer_local{0, 0, layer->texture_width, layer->texture_height};
    if (is_empty(layer_local))
        return {};
    const FbRect scissor_bounds = active_scissor_bounds(scissor, layer_bounds);
    if (is_empty(scissor_bounds))
        return layer_local;
    return intersect(layer_local, scissor_bounds);
}

uint32_t pack_abgr(Rml::ColourbPremultiplied colour)
{
    return (uint32_t(colour.alpha) << 24u) | (uint32_t(colour.blue) << 16u) |
           (uint32_t(colour.green) << 8u) | uint32_t(colour.red);
}

void premultiply_rgba(std::vector<uint8_t>& rgba)
{
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
        const uint32_t alpha = rgba[i + 3];
        rgba[i + 0] = static_cast<uint8_t>((uint32_t(rgba[i + 0]) * alpha + 127u) / 255u);
        rgba[i + 1] = static_cast<uint8_t>((uint32_t(rgba[i + 1]) * alpha + 127u) / 255u);
        rgba[i + 2] = static_cast<uint8_t>((uint32_t(rgba[i + 2]) * alpha + 127u) / 255u);
    }
}

Rml::Rectanglei clamp_scissor(Rml::Rectanglei region, int width, int height)
{
    const int left = std::clamp(region.Left(), 0, std::max(width, 0));
    const int top = std::clamp(region.Top(), 0, std::max(height, 0));
    const int right = std::clamp(region.Right(), 0, std::max(width, 0));
    const int bottom = std::clamp(region.Bottom(), 0, std::max(height, 0));
    if (right <= left || bottom <= top) {
        return Rml::Rectanglei::FromPositionSize({0, 0}, {0, 0});
    }
    return Rml::Rectanglei::FromPositionSize({left, top}, {right - left, bottom - top});
}

Rml::Rectanglei logical_scissor_to_framebuffer(Rml::Rectanglei region,
                                               const SurfaceMetrics& surface)
{
    const LogicalRect logical{static_cast<float>(region.Left()), static_cast<float>(region.Top()),
                              static_cast<float>(region.Width()),
                              static_cast<float>(region.Height())};
    const FbRect physical = logical_to_framebuffer(logical, surface);
    return Rml::Rectanglei::FromPositionSize({physical.x, physical.y}, {physical.w, physical.h});
}

std::array<float, 4> color_to_float(Rml::ColourbPremultiplied color)
{
    return {float(color.red) / 255.0f, float(color.green) / 255.0f, float(color.blue) / 255.0f,
            float(color.alpha) / 255.0f};
}

bool apply_color_stops(GradientRecord& gradient, const Rml::Dictionary& parameters)
{
    auto it = parameters.find("color_stop_list");
    if (it == parameters.end() || it->second.GetType() != Rml::Variant::COLORSTOPLIST)
        return false;
    const Rml::ColorStopList& stops = it->second.GetReference<Rml::ColorStopList>();
    gradient.stop_count = std::min<uint32_t>(uint32_t(stops.size()), kGradientStopLimit);
    for (uint32_t i = 0; i < gradient.stop_count; ++i) {
        gradient.stops[i].position = stops[i].position.number;
        gradient.stops[i].color = color_to_float(stops[i].color);
    }
    return gradient.stop_count > 0;
}

bool populate_gradient(GradientRecord& gradient, const Rml::String& name,
                       const Rml::Dictionary& parameters)
{
    const bool repeating = Rml::Get(parameters, "repeating", false);
    if (name == "linear-gradient") {
        gradient.kind = repeating ? GradientKind::RepeatingLinear : GradientKind::Linear;
        const Rml::Vector2f p0 = Rml::Get(parameters, "p0", Rml::Vector2f(0.0f));
        const Rml::Vector2f p1 = Rml::Get(parameters, "p1", Rml::Vector2f(0.0f));
        gradient.p_v = {p0.x, p0.y, p1.x - p0.x, p1.y - p0.y};
    } else if (name == "radial-gradient") {
        gradient.kind = repeating ? GradientKind::RepeatingRadial : GradientKind::Radial;
        const Rml::Vector2f center = Rml::Get(parameters, "center", Rml::Vector2f(0.0f));
        const Rml::Vector2f radius = Rml::Get(parameters, "radius", Rml::Vector2f(1.0f));
        gradient.p_v = {center.x, center.y, 1.0f / std::max(radius.x, 0.000001f),
                        1.0f / std::max(radius.y, 0.000001f)};
    } else if (name == "conic-gradient") {
        gradient.kind = repeating ? GradientKind::RepeatingConic : GradientKind::Conic;
        const Rml::Vector2f center = Rml::Get(parameters, "center", Rml::Vector2f(0.0f));
        const float angle = Rml::Get(parameters, "angle", 0.0f);
        gradient.p_v = {center.x, center.y, std::cos(angle), std::sin(angle)};
    } else {
        return false;
    }
    return apply_color_stops(gradient, parameters);
}

ClipOperationPlan clip_operation_plan(Rml::ClipMaskOperation operation)
{
    switch (operation) {
    case Rml::ClipMaskOperation::Set:
        return ClipOperationPlan::Set;
    case Rml::ClipMaskOperation::SetInverse:
        return ClipOperationPlan::SetInverse;
    case Rml::ClipMaskOperation::Intersect:
        return ClipOperationPlan::Intersect;
    }
    return ClipOperationPlan::Set;
}

} // namespace

struct RenderInterface::Impl {
    explicit Impl(const RendererConfig& config)
        : shader_provider(config.shaders), textures_provider(config.textures),
          diagnostics(config.diagnostics), material_shader_provider(config.material_shaders),
          perf_logger(config.perf_logger), render_path(config.render_path),
          blur_sample_bounds_mode(config.blur_sample_bounds_mode),
          reference_msaa_samples(config.reference_msaa_samples),
          trace_filter_pipeline(config.trace_filter_pipeline),
          pass_builder(config.views.begin, config.views.end, &perf),
          perf_logging_enabled(config.enable_perf_logging)
    {
        layout.begin()
            .add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
            .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
            .end();
        fullscreen_layout.begin()
            .add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
            .end();

        program = load_system_program(SystemProgram::RmlUi);
        composite_program = load_system_program(SystemProgram::Composite);
        composite_filter_program = load_system_program(SystemProgram::CompositeFilter);
        copy_program = load_system_program(SystemProgram::Copy);
        opacity_program = load_system_program(SystemProgram::Opacity);
        color_matrix_program = load_system_program(SystemProgram::ColorMatrix);
        mask_multiply_program = load_system_program(SystemProgram::MaskMultiply);
        blur_program = load_system_program(SystemProgram::Blur);
        drop_shadow_program = load_system_program(SystemProgram::DropShadow);
        gradient_program = load_system_program(SystemProgram::Gradient);
        sampler = bgfx::createUniform("s_texColor", bgfx::UniformType::Sampler);
        mask_sampler = bgfx::createUniform("s_mask", bgfx::UniformType::Sampler);
        mask_texcoord_transform_uniform =
            bgfx::createUniform("u_maskTexCoordTransform", bgfx::UniformType::Vec4);
        projection_uniform = bgfx::createUniform("u_projection", bgfx::UniformType::Mat4);
        transform_uniform = bgfx::createUniform("u_transform", bgfx::UniformType::Mat4);
        translate_uniform = bgfx::createUniform("u_translate", bgfx::UniformType::Vec4);
        color_matrix_uniform = bgfx::createUniform("u_colorMatrix", bgfx::UniformType::Mat4);
        opacity_uniform = bgfx::createUniform("u_opacity", bgfx::UniformType::Vec4);
        gradient_params_uniform =
            bgfx::createUniform("u_gradientParams", bgfx::UniformType::Vec4, 2);
        gradient_stops_uniform =
            bgfx::createUniform("u_gradientStops", bgfx::UniformType::Vec4, kGradientStopLimit);
        gradient_stop_meta_uniform =
            bgfx::createUniform("u_gradientStopMeta", bgfx::UniformType::Vec4, 4);
        blur_params_uniform = bgfx::createUniform("u_blurParams", bgfx::UniformType::Vec4);
        blur_weights_uniform = bgfx::createUniform("u_blurWeights", bgfx::UniformType::Vec4);
        texcoord_bounds_uniform = bgfx::createUniform("u_texCoordBounds", bgfx::UniformType::Vec4);
        shadow_color_uniform = bgfx::createUniform("u_shadowColor", bgfx::UniformType::Vec4);
        shadow_offset_uniform = bgfx::createUniform("u_shadowOffset", bgfx::UniformType::Vec4);

        const uint8_t white[] = {255, 255, 255, 255};
        white_texture = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::RGBA8,
                                              kRmlTextureFlags, bgfx::copy(white, sizeof(white)));
        reference_renderer.set_context(reference_context());
        resize(config.surface);
    }

    ~Impl()
    {
        for (auto& [_, geometry] : geometries) {
            destroy_geometry(geometry);
        }
        for (auto& [_, shader] : shaders) {
            release_shader_record(shader);
        }
        shaders.clear();
        for (auto& [_, texture] : textures) {
            if (texture_ownership_releases_handle(texture.ownership) &&
                bgfx::isValid(texture.handle)) {
                bgfx::destroy(texture.handle);
            }
        }
        destroy_layers();
        destroy_postprocess_targets();
        destroy_fullscreen_geometry();
        if (bgfx::isValid(white_texture))
            bgfx::destroy(white_texture);
        if (bgfx::isValid(program))
            bgfx::destroy(program);
        if (bgfx::isValid(composite_program))
            bgfx::destroy(composite_program);
        if (bgfx::isValid(composite_filter_program))
            bgfx::destroy(composite_filter_program);
        if (bgfx::isValid(copy_program))
            bgfx::destroy(copy_program);
        if (bgfx::isValid(opacity_program))
            bgfx::destroy(opacity_program);
        if (bgfx::isValid(color_matrix_program))
            bgfx::destroy(color_matrix_program);
        if (bgfx::isValid(mask_multiply_program))
            bgfx::destroy(mask_multiply_program);
        if (bgfx::isValid(blur_program))
            bgfx::destroy(blur_program);
        if (bgfx::isValid(drop_shadow_program))
            bgfx::destroy(drop_shadow_program);
        if (bgfx::isValid(gradient_program))
            bgfx::destroy(gradient_program);
        if (bgfx::isValid(sampler))
            bgfx::destroy(sampler);
        if (bgfx::isValid(mask_sampler))
            bgfx::destroy(mask_sampler);
        if (bgfx::isValid(projection_uniform))
            bgfx::destroy(projection_uniform);
        if (bgfx::isValid(transform_uniform))
            bgfx::destroy(transform_uniform);
        if (bgfx::isValid(translate_uniform))
            bgfx::destroy(translate_uniform);
        if (bgfx::isValid(color_matrix_uniform))
            bgfx::destroy(color_matrix_uniform);
        if (bgfx::isValid(opacity_uniform))
            bgfx::destroy(opacity_uniform);
        if (bgfx::isValid(gradient_params_uniform))
            bgfx::destroy(gradient_params_uniform);
        if (bgfx::isValid(gradient_stops_uniform))
            bgfx::destroy(gradient_stops_uniform);
        if (bgfx::isValid(gradient_stop_meta_uniform))
            bgfx::destroy(gradient_stop_meta_uniform);
        if (bgfx::isValid(blur_params_uniform))
            bgfx::destroy(blur_params_uniform);
        if (bgfx::isValid(blur_weights_uniform))
            bgfx::destroy(blur_weights_uniform);
        if (bgfx::isValid(texcoord_bounds_uniform))
            bgfx::destroy(texcoord_bounds_uniform);
        if (bgfx::isValid(mask_texcoord_transform_uniform))
            bgfx::destroy(mask_texcoord_transform_uniform);
        if (bgfx::isValid(shadow_color_uniform))
            bgfx::destroy(shadow_color_uniform);
        if (bgfx::isValid(shadow_offset_uniform))
            bgfx::destroy(shadow_offset_uniform);
    }

    bgfx::ProgramHandle load_system_program(SystemProgram requested_program)
    {
        if (!shader_provider) {
            log_error("shader provider is not configured");
            return BGFX_INVALID_HANDLE;
        }
        return shader_provider->load_program(requested_program);
    }

    void release_shader_record(ShaderRecord& shader)
    {
        if (shader.kind == ShaderRecordKind::Material && shader.material.valid() &&
            material_shader_provider) {
            material_shader_provider->release_decorator_shader(shader.material);
        }
        shader = {};
    }

    void log_warning(std::string_view message) const
    {
        if (diagnostics) {
            diagnostics->warning(message);
            return;
        }
        std::fprintf(stderr, "[rmlui] %.*s\n", int(message.size()), message.data());
    }

    void log_error(std::string_view message) const
    {
        if (diagnostics) {
            diagnostics->error(message);
            return;
        }
        std::fprintf(stderr, "[rmlui] %.*s\n", int(message.size()), message.data());
    }

    void log_perf_line(std::string_view message) const
    {
        if (perf_logger) {
            perf_logger->log_perf_line(message);
            return;
        }
        std::fprintf(stderr, "%.*s\n", int(message.size()), message.data());
    }

    void resize(const SurfaceMetrics& new_surface)
    {
        surface = sanitize_surface_metrics(new_surface);
        width = surface.framebuffer_width;
        height = surface.framebuffer_height;
        logical_width = surface.logical_width;
        logical_height = surface.logical_height;
        bx::mtxOrtho(projection, 0.0f, float(logical_width), float(logical_height), 0.0f, -10000.0f,
                     10000.0f, 0.0f, bgfx::getCaps()->homogeneousDepth);
        destroy_layers();
        destroy_postprocess_targets();
        reference_renderer.resize();
    }

    static void destroy_geometry(GeometryRecord& geometry)
    {
        if (bgfx::isValid(geometry.vb))
            bgfx::destroy(geometry.vb);
        if (bgfx::isValid(geometry.ib))
            bgfx::destroy(geometry.ib);
        geometry = {};
    }

    void destroy_fullscreen_geometry()
    {
        if (bgfx::isValid(fullscreen_vb))
            bgfx::destroy(fullscreen_vb);
        fullscreen_vb = BGFX_INVALID_HANDLE;
    }

    Rml::TextureHandle create_texture_from_rgba(std::vector<uint8_t> rgba, int tex_width,
                                                int tex_height, bool already_premultiplied)
    {
        if (tex_width <= 0 || tex_height <= 0 || tex_width > UINT16_MAX ||
            tex_height > UINT16_MAX) {
            return 0;
        }
        const bgfx::Caps* caps = bgfx::getCaps();
        if (caps && (uint32_t(tex_width) > caps->limits.maxTextureSize ||
                     uint32_t(tex_height) > caps->limits.maxTextureSize)) {
            std::fprintf(stderr, "[rmlui] texture too large: %dx%d max=%u\n", tex_width, tex_height,
                         caps->limits.maxTextureSize);
            return 0;
        }
        const size_t expected_size = size_t(tex_width) * size_t(tex_height) * 4u;
        if (rgba.size() != expected_size || expected_size > std::numeric_limits<uint32_t>::max()) {
            return 0;
        }
        if (!already_premultiplied) {
            premultiply_rgba(rgba);
        }
        bgfx::TextureHandle texture = bgfx::createTexture2D(
            uint16_t(tex_width), uint16_t(tex_height), false, 1, bgfx::TextureFormat::RGBA8,
            kRmlTextureFlags, bgfx::copy(rgba.data(), uint32_t(rgba.size())));
        if (!bgfx::isValid(texture)) {
            return 0;
        }
        const Rml::TextureHandle handle = ++texture_counter;
        textures.emplace(
            handle, TextureRecord{texture,
                                  {tex_width, tex_height},
                                  RenderBounds{{0.0f, 0.0f, float(tex_width), float(tex_height)},
                                               {0, 0, tex_width, tex_height}},
                                  TextureOwnership::External});
        return handle;
    }

    bgfx::TextureFormat::Enum depth_stencil_format() const
    {
        // Probe once and cache. WebGL's getInternalformatParameter is slow and
        // generates console spam when called repeatedly.
        if (!stencil_cached) {
            stencil_cached = true;
            const bool d24s8 = bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::D24S8,
                                                    BGFX_TEXTURE_RT_WRITE_ONLY);
            const bool d0s8 = bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::D0S8,
                                                   BGFX_TEXTURE_RT_WRITE_ONLY);
            switch (choose_stencil_plan(d24s8, d0s8)) {
            case StencilPlan::D24S8:
            case StencilPlan::StencilAttachment:
                cached_stencil_format = bgfx::TextureFormat::D24S8;
                break;
            case StencilPlan::D0S8:
                cached_stencil_format = bgfx::TextureFormat::D0S8;
                break;
            case StencilPlan::Unsupported:
                cached_stencil_format = bgfx::TextureFormat::Unknown;
                break;
            }
        }
        return cached_stencil_format;
    }

    void destroy_layer(LayerRecord& layer) { target_cache.destroy_layer(layer); }

    void destroy_render_target(RenderTargetRecord& target)
    {
        target_cache.destroy_render_target(target);
    }

    Rml::Rectanglei current_save_bounds()
    {
        LayerRecord* layer = current_layer();
        if (!layer) {
            return Rml::Rectanglei::FromPositionSize({0, 0}, {0, 0});
        }

        // RmlUi's SaveLayerAsTexture contract is based on the current scissor region. This
        // is especially important for callback textures such as box-shadow generation, where
        // RmlUi sets the scissor to the desired texture dimensions before pushing a temporary
        // layer. Do not derive the save region from recorded content bounds, or the generated
        // texture can be cropped to the element/background bounds while RmlUi still renders it
        // using the full callback texture geometry.
        FbRect bounds{};
        if (scissor_enabled) {
            const Rml::Rectanglei clipped = clamp_scissor(scissor_region, width, height);
            if (clipped.Width() <= 0 || clipped.Height() <= 0) {
                return Rml::Rectanglei::FromPositionSize({0, 0}, {0, 0});
            }
            bounds = {clipped.Left(), clipped.Top(), clipped.Width(), clipped.Height()};
        } else if (!is_empty(layer->bounds.framebuffer)) {
            bounds = layer->bounds.framebuffer;
        } else {
            bounds = {0, 0, width, height};
        }

        // Keep the requested save rectangle intact. A virtual layer may already be materialized
        // to tight content bounds by CompositeLayers, but SaveLayerAsTexture still has to return
        // a texture with the current scissor dimensions, matching GL3's full-layer framebuffer
        // behavior. Clipping this to layer->bounds would make callback textures look stretched.
        bounds = clamp_to_surface(align_outward_for_render_target(bounds), surface);
        if (is_empty(bounds)) {
            return Rml::Rectanglei::FromPositionSize({0, 0}, {0, 0});
        }
        return rectangle_from_fb(bounds);
    }

    void destroy_layers()
    {
        target_cache.destroy_layers();
        layer_system.clear_stack_to_base();
    }

    bool recorded_geometry_reference_exists(Rml::CompiledGeometryHandle geometry) const
    {
        for (const LayerRecord& layer : layers) {
            for (const RecordedDrawCommand& command : layer.commands) {
                if ((command.kind == RecordedCommandKind::Geometry ||
                     command.kind == RecordedCommandKind::Shader ||
                     command.kind == RecordedCommandKind::ClipMask) &&
                    command.geometry == geometry) {
                    return true;
                }
            }
        }
        for (const ClipCommand& command : clip_commands) {
            if (command.geometry == geometry) {
                return true;
            }
        }
        return false;
    }

    void release_deferred_geometries()
    {
        for (Rml::CompiledGeometryHandle handle : deferred_geometry_release) {
            if (auto it = geometries.find(handle); it != geometries.end()) {
                destroy_geometry(it->second);
                geometries.erase(it);
            }
        }
        deferred_geometry_release.clear();
    }

    void destroy_postprocess_targets() { target_cache.destroy_postprocess_targets(); }

    RenderBounds compute_child_layer_bounds(Rml::LayerHandle parent_handle,
                                            const ScissorState& captured_scissor,
                                            bool captured_transform_valid,
                                            bool count_fallbacks = true) const
    {
        const RenderBounds* parent_ptr = nullptr;
        if (size_t(parent_handle) < layers.size()) {
            parent_ptr = &layers[size_t(parent_handle)].bounds;
        }

        const FbRect* scissor_ptr = nullptr;
        FbRect scissor_fb{};
        if (captured_scissor.enabled) {
            const Rml::Rectanglei clipped = clamp_scissor(captured_scissor.region, width, height);
            if (clipped.Width() > 0 && clipped.Height() > 0) {
                scissor_fb = {clipped.Left(), clipped.Top(), clipped.Width(), clipped.Height()};
                scissor_ptr = &scissor_fb;
            }
        }
        if (count_fallbacks && (!scissor_ptr || captured_transform_valid)) {
            const_cast<rmlui_bgfx::PerfCounters&>(perf).add_unbounded_layer_fallback(
                !scissor_ptr, captured_transform_valid);
        }

        return rmlui_bgfx::compute_child_layer_bounds(surface, parent_ptr, scissor_ptr,
                                                      captured_transform_valid);
    }

    RenderBounds compute_child_layer_bounds(Rml::LayerHandle parent_handle) const
    {
        return compute_child_layer_bounds(
            parent_handle, ScissorState{scissor_enabled, scissor_region}, transform_valid);
    }

    bool ensure_layer(size_t index, const RenderBounds& bounds)
    {
        if (index > uint32_t(LayerPoolPlan::InvalidLayer - 1u))
            return false;
        return target_cache.ensure_layer_target(uint32_t(index), bounds, depth_stencil_format(),
                                                reference_msaa_samples);
    }

    LayerRecord* layer_for_handle(Rml::LayerHandle handle)
    {
        return layer_system.layer_for_handle(handle);
    }

    LayerRecord* materialized_layer_for_handle(Rml::LayerHandle handle)
    {
        return layer_system.materialized_layer_for_handle(handle, direct_base_requested);
    }

    LayerRecord* current_layer() { return layer_system.current_layer(); }

    bool active_layer_is_recording() const { return layer_system.active_layer_is_recording(); }

    FbRect scissor_fb_bounds(const ScissorState& state) const
    {
        if (!state.enabled)
            return {};
        const Rml::Rectanglei region = clamp_scissor(state.region, width, height);
        if (region.Width() <= 0 || region.Height() <= 0)
            return {};
        return {region.Left(), region.Top(), region.Width(), region.Height()};
    }

    FbRect layer_limit_bounds(const LayerRecord& layer) const
    {
        FbRect bounds{0, 0, width, height};
        if (size_t(layer.parent_layer) < layers.size()) {
            const FbRect parent_bounds = layers[size_t(layer.parent_layer)].bounds.framebuffer;
            if (!is_empty(parent_bounds))
                bounds = intersect(bounds, parent_bounds);
        }
        if (layer.push_scissor.enabled) {
            const FbRect scissor_bounds = scissor_fb_bounds(layer.push_scissor);
            if (!is_empty(scissor_bounds))
                bounds = intersect(bounds, scissor_bounds);
        }
        if (!is_empty(layer.bounds.framebuffer))
            bounds = intersect(bounds, layer.bounds.framebuffer);
        return bounds;
    }

    RenderBounds bounds_from_framebuffer_rect(FbRect framebuffer_bounds) const
    {
        RenderBounds bounds;
        bounds.framebuffer =
            clamp_to_surface(align_outward_for_render_target(framebuffer_bounds), surface);
        if (is_empty(bounds.framebuffer)) {
            bounds.framebuffer = {0, 0, 1, 1};
        }
        bounds.logical = framebuffer_to_logical(bounds.framebuffer, surface);
        return bounds;
    }

    FbRect layer_recorded_content_bounds(const LayerRecord& layer) const
    {
        FbRect content = layer.has_valid_content_bounds ? layer.valid_content_bounds : FbRect{};
        const FbRect limit = layer_limit_bounds(layer);
        if (layer.content_bounds_transform_fallback || layer.content_bounds_inverse_mask_fallback) {
            content = limit;
        } else if (!is_empty(content) && !is_empty(limit)) {
            content = intersect(content, limit);
        }
        return clamp_to_surface(align_outward_for_render_target(content), surface);
    }

    RenderBounds choose_materialized_layer_bounds(const LayerRecord& layer,
                                                  std::optional<FbRect> required_bounds) const
    {
        const bool has_recorded_transform =
            std::any_of(layer.commands.begin(), layer.commands.end(),
                        [](const RecordedDrawCommand& command) { return command.transform_valid; });
        if (layer.push_transform_valid || has_recorded_transform) {
            // Transforms are evaluated in render space after virtual-layer recording. A bounded
            // target changes that coordinate space unless every draw and clip is rebased to the
            // target origin, so retain the reference renderer's full-frame contract for now.
            return bounds_from_framebuffer_rect({0, 0, width, height});
        }
        FbRect saved_texture_bounds{};
        for (const RecordedDrawCommand& command : layer.commands) {
            const auto texture_it = textures.find(command.texture);
            if (texture_it == textures.end() ||
                texture_it->second.ownership != TextureOwnership::SavedLayer) {
                continue;
            }
            const auto command_bounds =
                command_fb_bounds(command.geometry, command.translation, ScissorState{},
                                  command.transform_valid, command.transform);
            if (command_bounds && !is_empty(*command_bounds)) {
                saved_texture_bounds = is_empty(saved_texture_bounds)
                                           ? *command_bounds
                                           : union_rects(saved_texture_bounds, *command_bounds);
            }
        }
        if (!is_empty(saved_texture_bounds)) {
            if (layer.push_transform_valid) {
                // The layer transform is applied after recording. A compact target in untransformed
                // content coordinates cannot preserve a saved texture's render-space contract.
                // A future optimization can use transformed callback-quad bounds if it also rebases
                // every geometry, clip-mask, and composite operation into that target's origin.
                return bounds_from_framebuffer_rect({0, 0, width, height});
            }
            // Preserve the callback quad's complete coordinate space without paying for a
            // full-frame target. Parent clipping is still applied when the layer is composited.
            if (required_bounds && !is_empty(*required_bounds)) {
                const FbRect required =
                    clamp_to_surface(align_outward_for_render_target(*required_bounds), surface);
                saved_texture_bounds = union_rects(saved_texture_bounds, required);
            }
            return bounds_from_framebuffer_rect(saved_texture_bounds);
        }
        FbRect selected = layer_recorded_content_bounds(layer);
        if (required_bounds && !is_empty(*required_bounds)) {
            const FbRect required =
                clamp_to_surface(align_outward_for_render_target(*required_bounds), surface);
            selected = is_empty(selected) ? required : union_rects(selected, required);
        }

        FbRect limit = layer_limit_bounds(layer);
        if (required_bounds && !is_empty(*required_bounds)) {
            const FbRect required =
                clamp_to_surface(align_outward_for_render_target(*required_bounds), surface);
            limit = is_empty(limit) ? required : union_rects(limit, required);
        }
        if (!is_empty(limit) && !is_empty(selected)) {
            selected = intersect(selected, limit);
        }
        selected = clamp_to_surface(align_outward_for_render_target(selected), surface);
        if (is_empty(selected)) {
            FbRect fallback = !is_empty(limit) ? limit : FbRect{0, 0, width, height};
            fallback = clamp_to_surface(fallback, surface);
            const int x = std::clamp(fallback.x, 0, std::max(width - 1, 0));
            const int y = std::clamp(fallback.y, 0, std::max(height - 1, 0));
            selected = {x, y, 1, 1};
        }
        return bounds_from_framebuffer_rect(selected);
    }

    std::optional<FbRect> geometry_fb_bounds(const GeometryRecord& geometry,
                                             Rml::Vector2f translation, const ScissorState& state,
                                             bool command_transform_valid,
                                             const std::array<float, 16>& command_transform) const
    {
        Rml::Matrix4f transform_matrix;
        const Rml::Matrix4f* transform_ptr = nullptr;
        if (command_transform_valid) {
            std::memcpy(transform_matrix.data(), command_transform.data(), sizeof(float) * 16);
            transform_ptr = &transform_matrix;
        }

        const GeometryBoundsResult geometry_bounds = compute_transformed_geometry_bounds(
            geometry.local_bounds, translation, transform_ptr, surface);
        if (geometry_bounds.status == GeometryBoundsStatus::EmptyGeometry)
            return FbRect{};
        if (geometry_bounds.status != GeometryBoundsStatus::Valid)
            return std::nullopt;

        FbRect bounds = geometry_bounds.framebuffer;
        if (state.enabled) {
            const FbRect state_bounds = scissor_fb_bounds(state);
            if (is_empty(state_bounds))
                return FbRect{};
            bounds = intersect(bounds, state_bounds);
        }
        return bounds;
    }

    std::optional<FbRect> command_fb_bounds(Rml::CompiledGeometryHandle geometry,
                                            Rml::Vector2f translation, const ScissorState& state,
                                            bool command_transform_valid,
                                            const std::array<float, 16>& command_transform) const
    {
        auto it = geometries.find(geometry);
        if (it == geometries.end())
            return std::nullopt;
        return geometry_fb_bounds(it->second, translation, state, command_transform_valid,
                                  command_transform);
    }

    void add_layer_content_region(LayerRecord& layer, std::optional<FbRect> maybe_bounds,
                                  bool bounds_transform_valid, bool clip_mask_enabled)
    {
        if (!maybe_bounds) {
            if (bounds_transform_valid)
                layer.content_bounds_transform_fallback = true;
            return;
        }
        FbRect bounds = *maybe_bounds;
        const FbRect container = layer_limit_bounds(layer);
        if (!is_empty(container))
            bounds = intersect(bounds, container);
        if (clip_mask_enabled)
            bounds = apply_mask_constraints(bounds, nullptr, &layer.conservative_mask_bounds);
        if (is_empty(bounds))
            return;
        layer.valid_content_bounds = layer.has_valid_content_bounds
                                         ? union_rects(layer.valid_content_bounds, bounds)
                                         : bounds;
        layer.has_valid_content_bounds = true;
    }

    void add_recorded_region(LayerRecord& layer, const RecordedDrawCommand& command)
    {
        add_layer_content_region(layer,
                                 command_fb_bounds(command.geometry, command.translation,
                                                   command.scissor, command.transform_valid,
                                                   command.transform),
                                 command.transform_valid, command.clip_mask_enabled);
    }

    void add_submitted_region(LayerRecord& layer, const GeometryRecord& geometry,
                              Rml::Vector2f translation)
    {
        const ScissorState scissor{scissor_enabled, scissor_region};
        std::array<float, 16> active_transform{};
        if (transform_valid) {
            std::memcpy(active_transform.data(), transform, sizeof(transform));
        }
        add_layer_content_region(
            layer,
            geometry_fb_bounds(geometry, translation, scissor, transform_valid, active_transform),
            transform_valid, layer.clip_mask_enabled);
    }

    void update_layer_mask_region(LayerRecord& layer, const ClipCommand& command)
    {
        const auto maybe_bounds =
            command_fb_bounds(command.geometry, command.translation, command.scissor,
                              command.transform_valid, command.transform);
        if (!maybe_bounds) {
            if (command.transform_valid)
                layer.content_bounds_transform_fallback = true;
            return;
        }
        FbRect fallback_bounds = layer_limit_bounds(layer);
        if (command.scissor.enabled) {
            const FbRect scissor_bounds = scissor_fb_bounds(command.scissor);
            if (!is_empty(scissor_bounds))
                fallback_bounds = intersect(fallback_bounds, scissor_bounds);
        }
        layer.conservative_mask_bounds = update_conservative_mask_bounds(
            layer.conservative_mask_bounds, command.operation, *maybe_bounds, fallback_bounds);
        if (command.operation == Rml::ClipMaskOperation::SetInverse &&
            layer.conservative_mask_bounds.inverse_fallback)
            layer.content_bounds_inverse_mask_fallback = true;
    }

    void record_geometry_command(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                                 Rml::TextureHandle texture)
    {
        LayerRecord* layer = current_layer();
        if (!layer)
            return;
        RecordedDrawCommand command;
        command.kind = RecordedCommandKind::Geometry;
        command.geometry = geometry;
        command.texture = texture;
        command.translation = translation;
        command.scissor = ScissorState{scissor_enabled, scissor_region};
        command.transform_valid = transform_valid;
        if (command.transform_valid) {
            std::memcpy(command.transform.data(), transform, sizeof(transform));
        }
        command.clip_mask_enabled = layer->clip_mask_enabled;
        command.stencil_ref = layer->stencil_ref;
        layer->commands.push_back(command);
        add_recorded_region(*layer, command);
    }

    void record_shader_command(Rml::CompiledShaderHandle shader,
                               Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                               Rml::TextureHandle texture)
    {
        LayerRecord* layer = current_layer();
        if (!layer)
            return;
        RecordedDrawCommand command;
        command.kind = RecordedCommandKind::Shader;
        command.shader = shader;
        command.geometry = geometry;
        command.texture = texture;
        command.translation = translation;
        command.scissor = ScissorState{scissor_enabled, scissor_region};
        command.transform_valid = transform_valid;
        if (command.transform_valid) {
            std::memcpy(command.transform.data(), transform, sizeof(transform));
        }
        command.clip_mask_enabled = layer->clip_mask_enabled;
        command.stencil_ref = layer->stencil_ref;
        layer->commands.push_back(command);
        add_recorded_region(*layer, command);
    }

    void record_clip_mask_command(const ClipCommand& clip_command)
    {
        LayerRecord* layer = current_layer();
        if (!layer)
            return;
        RecordedDrawCommand command;
        command.kind = RecordedCommandKind::ClipMask;
        command.geometry = clip_command.geometry;
        command.translation = clip_command.translation;
        command.scissor = clip_command.scissor;
        command.transform_valid = clip_command.transform_valid;
        command.transform = clip_command.transform;
        command.clip_mask_enabled = layer->clip_mask_enabled;
        command.stencil_ref = clip_command.previous_ref;
        command.clip_operation = clip_command.operation;
        command.previous_ref = clip_command.previous_ref;
        command.next_ref = clip_command.next_ref;
        layer->commands.push_back(command);
        update_layer_mask_region(*layer, clip_command);
        layer->stencil_ref = clip_command.next_ref;
        layer->clip_commands.push_back(clip_commands.size());
        clip_commands.push_back(clip_command);
    }

    bool clear_materialized_layer(Rml::LayerHandle handle, bool is_bounded)
    {
        LayerRecord* layer = materialized_layer_for_handle(handle);
        if (!layer)
            return false;
        const int lw = layer->texture_width;
        const int lh = layer->texture_height;
        auto pass = pass_builder.layer_clear(layer->framebuffer, lw, lh);
        if (!pass)
            return false;
        perf.add_layer_clear();
        perf.add_clear(uint64_t(lw) * uint64_t(lh), !is_bounded);
        bgfx::touch(pass->view);
        return true;
    }

    bool replay_recorded_commands(Rml::LayerHandle handle)
    {
        LayerRecord* layer = materialized_layer_for_handle(handle);
        if (!layer)
            return false;
        const std::vector<RecordedDrawCommand> commands = layer->commands;
        const bool final_clip_mask_enabled = layer->clip_mask_enabled;
        const uint8_t final_stencil_ref = layer->stencil_ref;
        const Rml::LayerHandle saved_active = active_layer;
        const bool saved_scissor_enabled = scissor_enabled;
        const Rml::Rectanglei saved_scissor_region = scissor_region;
        const bool saved_transform_valid = transform_valid;
        const std::array<float, 16> saved_transform = [&] {
            std::array<float, 16> copy{};
            std::memcpy(copy.data(), transform, sizeof(transform));
            return copy;
        }();

        active_layer = handle;
        if (trace_filter_pipeline) {
            std::fprintf(stderr,
                         "[rmlui-bgfx][replay] begin layer=%zu commands=%zu bounds=(%d,%d %dx%d) "
                         "texture=%dx%d clips=%zu\n",
                         size_t(handle), commands.size(), layer->bounds.framebuffer.x,
                         layer->bounds.framebuffer.y, layer->bounds.framebuffer.w,
                         layer->bounds.framebuffer.h, layer->texture_width, layer->texture_height,
                         layer->clip_commands.size());
        }
        size_t command_index = 0;
        for (const RecordedDrawCommand& command : commands) {
            LayerRecord* replay_layer = materialized_layer_for_handle(handle);
            if (!replay_layer)
                break;
            scissor_enabled = command.scissor.enabled;
            scissor_region = command.scissor.region;
            transform_valid = command.transform_valid;
            if (transform_valid) {
                std::memcpy(transform, command.transform.data(), sizeof(transform));
            }
            replay_layer->clip_mask_enabled = command.clip_mask_enabled;
            replay_layer->stencil_ref = command.stencil_ref;
            if (trace_filter_pipeline) {
                const auto bounds =
                    command_fb_bounds(command.geometry, command.translation, command.scissor,
                                      command.transform_valid, command.transform);
                std::fprintf(stderr,
                             "[rmlui-bgfx][replay] layer=%zu cmd=%zu kind=%d geom=%zu shader=%zu "
                             "tex=%zu transform=%d clip=%d ref=%u scissor=%d",
                             size_t(handle), command_index, int(command.kind),
                             size_t(command.geometry), size_t(command.shader),
                             size_t(command.texture), command.transform_valid ? 1 : 0,
                             command.clip_mask_enabled ? 1 : 0, unsigned(command.stencil_ref),
                             command.scissor.enabled ? 1 : 0);
                if (bounds) {
                    std::fprintf(stderr, " bounds=(%d,%d %dx%d)", bounds->x, bounds->y, bounds->w,
                                 bounds->h);
                } else {
                    std::fprintf(stderr, " bounds=<fallback>");
                }
                if (command.scissor.enabled) {
                    std::fprintf(stderr, " scissor_rect=(%d,%d %dx%d)",
                                 command.scissor.region.Left(), command.scissor.region.Top(),
                                 command.scissor.region.Width(), command.scissor.region.Height());
                }
                std::fprintf(stderr, "\n");
            }

            switch (command.kind) {
            case RecordedCommandKind::Geometry: {
                auto geometry_it = geometries.find(command.geometry);
                if (geometry_it != geometries.end()) {
                    submit(geometry_it->second, command.translation, command.texture);
                }
                break;
            }
            case RecordedCommandKind::Shader: {
                auto shader_it = shaders.find(command.shader);
                auto geometry_it = geometries.find(command.geometry);
                if (shader_it != shaders.end() && geometry_it != geometries.end()) {
                    submit_shader(shader_it->second, geometry_it->second, command.translation,
                                  command.texture);
                }
                break;
            }
            case RecordedCommandKind::ClipMask: {
                ClipCommand clip_command;
                clip_command.operation = command.clip_operation;
                clip_command.geometry = command.geometry;
                clip_command.translation = command.translation;
                clip_command.scissor = command.scissor;
                clip_command.transform_valid = command.transform_valid;
                clip_command.transform = command.transform;
                clip_command.previous_ref = command.previous_ref;
                clip_command.next_ref = command.next_ref;
                apply_clip_command(clip_command, false);
                break;
            }
            }
            ++command_index;
        }

        if (LayerRecord* final_layer = materialized_layer_for_handle(handle)) {
            final_layer->clip_mask_enabled = final_clip_mask_enabled;
            final_layer->stencil_ref = final_stencil_ref;
        }
        active_layer = saved_active;
        scissor_enabled = saved_scissor_enabled;
        scissor_region = saved_scissor_region;
        transform_valid = saved_transform_valid;
        std::memcpy(transform, saved_transform.data(), sizeof(transform));
        return !frame_failed;
    }

    BgfxLayerMaterializeContext materialize_context()
    {
        return BgfxLayerMaterializeContext{
            surface,
            [this](const LayerRecord& layer, std::optional<FbRect> required_bounds) {
                return choose_materialized_layer_bounds(layer, required_bounds);
            },
            [this](size_t handle, const RenderBounds& bounds) {
                if (!ensure_layer(handle, bounds)) {
                    fail_frame("failed to materialize virtual RmlUi layer");
                    return false;
                }
                if (LayerRecord* layer = layer_for_handle(Rml::LayerHandle(handle))) {
                    const bool is_bounded =
                        bounds.framebuffer.w < width || bounds.framebuffer.h < height;
                    perf.update_child_layer_max(uint32_t(layer->texture_width),
                                                uint32_t(layer->texture_height));
                    if (is_bounded) {
                        perf.add_bounded_child_layer();
                    } else {
                        perf.add_full_frame_child_layer();
                    }
                }
                return true;
            },
            [this](Rml::LayerHandle handle, bool bounded) {
                if (!clear_materialized_layer(handle, bounded)) {
                    fail_frame("failed to clear materialized RmlUi layer");
                    return false;
                }
                return true;
            },
            [this](Rml::LayerHandle handle, const std::vector<size_t>& commands) {
                replay_clip_commands(handle, commands);
            },
            [this](Rml::LayerHandle handle) { return replay_recorded_commands(handle); }};
    }

    bool materialize_layer(Rml::LayerHandle handle,
                           std::optional<FbRect> required_bounds = std::nullopt)
    {
        return layer_system.materialize_layer(materialize_context(), handle, required_bounds);
    }

    BgfxLayerSaveTextureContext save_texture_context()
    {
        return BgfxLayerSaveTextureContext{
            direct_base_requested,
            &root_requires_preservation,
            &textures,
            &texture_counter,
            [this](const char* message) { fail_frame(message); },
            [this](Rml::LayerHandle handle, std::optional<FbRect> required_bounds) {
                return materialize_layer(handle, required_bounds);
            },
            [this]() { return current_save_bounds(); },
            [this](bgfx::TextureHandle source, Rml::Rectanglei region, int source_width,
                   int source_height, const char* name, bool flip_y) {
                return copy_region_to_texture(source, region, source_width, source_height, name,
                                              flip_y);
            },
            [this](bgfx::TextureHandle source, Rml::Rectanglei region, int source_width,
                   int source_height, Rml::Vector2i output_dimensions,
                   Rml::Vector2i destination_offset, const char* name, bool flip_y) {
                return copy_region_to_sized_texture(source, region, source_width, source_height,
                                                    output_dimensions, destination_offset, name,
                                                    flip_y);
            }};
    }

    BgfxLayerSaveMaskContext save_mask_context()
    {
        return BgfxLayerSaveMaskContext{
            surface,
            direct_base_requested,
            &root_requires_preservation,
            &textures,
            &filters,
            &texture_counter,
            &filter_counter,
            [this](const char* message) { fail_frame(message); },
            [this](Rml::LayerHandle handle, std::optional<FbRect> required_bounds) {
                return materialize_layer(handle, required_bounds);
            },
            [this]() { return current_save_bounds(); },
            [this](bgfx::TextureHandle source, Rml::Rectanglei region, int source_width,
                   int source_height, const char* name, bool flip_y) {
                return copy_region_to_texture(source, region, source_width, source_height, name,
                                              flip_y);
            },
            [this](PostprocessTargetKind kind, const FbRect& bounds) {
                return ensure_postprocess_target(kind, bounds);
            },
            [this](const CompositeOp& op) { return composite(op); }};
    }

    bool begin_base_layer()
    {
        release_deferred_geometries();
        perf.reset();
        direct_base_presented = false;
        direct_base_fallback_reason = nullptr;
        const bgfx::Caps* caps = bgfx::getCaps();
        const bool direct_mode_capable =
            caps != nullptr && caps->rendererType != bgfx::RendererType::Noop;
        const bool stencil_capable = depth_stencil_format() != bgfx::TextureFormat::Unknown;
        const bool webgl_feedback_sensitive =
#if defined(__EMSCRIPTEN__)
            true;
#else
            false;
#endif
        const bool layer_msaa_requested =
            reference_msaa_samples == 2 || reference_msaa_samples == 4 ||
            reference_msaa_samples == 8 || reference_msaa_samples == 16;
        const auto policy = choose_base_presentation_policy(
            !base_direct_compatibility_enabled && !layer_msaa_requested, direct_mode_capable,
            root_requires_preservation, stencil_capable, webgl_feedback_sensitive);
        direct_base_requested = policy.mode == BasePresentationMode::DirectToBackbuffer;
        direct_base_fallback_reason = policy.fallback_reason;
        if (direct_base_requested) {
            perf.add_direct_base_presentation();
        } else {
            perf.add_offscreen_base_presentation();
            perf.add_direct_base_fallback();
            if (direct_base_fallback_reason &&
                logged_base_fallback_reason != direct_base_fallback_reason) {
                std::fprintf(stderr, "[rmlui] base presentation fallback: %s\n",
                             direct_base_fallback_reason);
                logged_base_fallback_reason = direct_base_fallback_reason;
            }
        }
        RenderBounds base_bounds;
        base_bounds.logical = {0.0f, 0.0f, float(logical_width), float(logical_height)};
        base_bounds.framebuffer = {0, 0, width, height};
        if (direct_base_requested) {
            if (layers.size() < 1)
                layers.resize(1);
            LayerRecord& base = layers[0];
            destroy_layer(base);
            base.bounds = base_bounds;
            base.texture_width = width;
            base.texture_height = height;
            base.clip_mask_enabled = false;
            base.stencil_ref = 1;
            base.clip_commands.clear();
            bx::mtxOrtho(base.projection, base_bounds.logical.x,
                         base_bounds.logical.x + base_bounds.logical.w,
                         base_bounds.logical.y + base_bounds.logical.h, base_bounds.logical.y,
                         -10000.0f, 10000.0f, 0.0f, caps ? caps->homogeneousDepth : false);
        } else {
            if (!ensure_layer(0, base_bounds))
                return false;
            perf.add_full_frame_layer();
        }
        layer_pool.begin_frame();
        layer_system.begin_frame();
        layers[0].kind = LayerKind::Root;
        layers[0].parent_layer = 0;
        layers[0].recording = false;
        layers[0].materialized = true;
        layers[0].clear_pending = false;
        layers[0].commands.clear();
        layers[0].valid_content_bounds = {};
        layers[0].has_valid_content_bounds = false;
        layers[0].conservative_mask_bounds = {};
        layers[0].content_bounds_transform_fallback = false;
        layers[0].content_bounds_inverse_mask_fallback = false;
        layers[0].clip_mask_enabled = false;
        layers[0].stencil_ref = 1;
        layers[0].clip_commands.clear();
        layers[0].inherited_clip_command_count = 0;
        clip_commands.clear();
        frame_failed = false;
        return true;
    }

    void fail_frame(const char* message)
    {
        if (!frame_failed && message) {
            std::fprintf(stderr, "[rmlui] %s\n", message);
        }
        frame_failed = true;
    }

    void submit(const GeometryRecord& geometry, Rml::Vector2f translation,
                Rml::TextureHandle texture)
    {
        if (frame_failed || !bgfx::isValid(program) || geometry.index_count == 0 ||
            pass_builder.exhausted()) {
            return;
        }
        LayerRecord* layer = current_layer();
        if (!layer)
            return;
        const int lw = layer->texture_width;
        const int lh = layer->texture_height;
        auto pass = pass_builder.geometry(layer->framebuffer, lw, lh);
        if (!pass)
            return;
        perf.add_geometry(uint64_t(lw) * uint64_t(lh), geometry.index_count);
        bgfx::TextureHandle bgfx_texture = white_texture;
        if (auto it = textures.find(texture); it != textures.end()) {
            bgfx_texture = it->second.handle;
        }
        draw_context.submit_geometry(
            *pass, draw_resources(), geometry, *layer,
            BgfxGeometryDrawState{translation, bgfx_texture,
                                  ScissorState{scissor_enabled, scissor_region}, transform_valid,
                                  transform, layer->clip_mask_enabled, layer->msaa_enabled,
                                  stencil_test_state()});
        add_submitted_region(*layer, geometry, translation);
    }

    bool ensure_fullscreen_geometry()
    {
        if (bgfx::isValid(fullscreen_vb))
            return true;
        const bool origin_bottom_left = bgfx::getCaps() && bgfx::getCaps()->originBottomLeft;
        const auto vertices = fullscreen_triangle(origin_bottom_left);
        fullscreen_vb = bgfx::createVertexBuffer(
            bgfx::copy(vertices.data(), uint32_t(vertices.size() * sizeof(FullscreenVertex))),
            fullscreen_layout);
        return bgfx::isValid(fullscreen_vb);
    }

    BgfxDrawResources draw_resources() const
    {
        return BgfxDrawResources{fullscreen_vb,
                                 white_texture,
                                 sampler,
                                 mask_sampler,
                                 projection_uniform,
                                 transform_uniform,
                                 translate_uniform,
                                 gradient_params_uniform,
                                 gradient_stops_uniform,
                                 gradient_stop_meta_uniform,
                                 texcoord_bounds_uniform,
                                 mask_texcoord_transform_uniform,
                                 opacity_uniform,
                                 color_matrix_uniform,
                                 blur_params_uniform,
                                 blur_weights_uniform,
                                 shadow_color_uniform,
                                 shadow_offset_uniform,
                                 program,
                                 composite_program,
                                 composite_filter_program,
                                 copy_program,
                                 gradient_program,
                                 mask_multiply_program,
                                 opacity_program,
                                 color_matrix_program,
                                 blur_program,
                                 drop_shadow_program,
                                 identity,
                                 kRmlBlendState};
    }

    ReferenceRendererContext reference_context()
    {
        ReferenceRendererContext context;
        context.geometries = &geometries;
        context.textures = &textures;
        context.filters = &filters;
        context.shaders = &shaders;
        context.texture_counter = &texture_counter;
        context.filter_counter = &filter_counter;
        context.pass_builder = &pass_builder;
        context.draw_context = &draw_context;
        context.perf = &perf;
        context.material_shaders = material_shader_provider;
        context.fullscreen_layout = &fullscreen_layout;
        context.white_texture = white_texture;
        context.programs = ReferenceRendererPrograms{program,
                                                     composite_program,
                                                     composite_filter_program,
                                                     copy_program,
                                                     opacity_program,
                                                     color_matrix_program,
                                                     mask_multiply_program,
                                                     blur_program,
                                                     drop_shadow_program,
                                                     gradient_program};
        context.uniforms = ReferenceRendererUniforms{sampler,
                                                     mask_sampler,
                                                     projection_uniform,
                                                     transform_uniform,
                                                     translate_uniform,
                                                     color_matrix_uniform,
                                                     opacity_uniform,
                                                     gradient_params_uniform,
                                                     gradient_stops_uniform,
                                                     gradient_stop_meta_uniform,
                                                     blur_params_uniform,
                                                     blur_weights_uniform,
                                                     texcoord_bounds_uniform,
                                                     mask_texcoord_transform_uniform,
                                                     shadow_color_uniform,
                                                     shadow_offset_uniform};
        context.identity = identity;
        context.premultiplied_blend_state = kRmlBlendState;
        context.reference_msaa_samples = reference_msaa_samples;
        context.trace = trace_filter_pipeline;
        return context;
    }

    BgfxFilterPipelineContext filter_context()
    {
        // BgfxFilterPipelineContext carries a draw-resource snapshot. Ensure the fullscreen
        // geometry exists before taking that snapshot, otherwise the filter copy/blur passes may
        // validate the geometry in the pipeline but still submit with an invalid cached handle.
        (void)ensure_fullscreen_geometry();
        return BgfxFilterPipelineContext{filters,
                                         textures,
                                         surface,
                                         target_cache,
                                         pass_builder,
                                         draw_context,
                                         draw_resources(),
                                         perf,
                                         render_path,
                                         blur_sample_bounds_mode,
                                         false,
                                         trace_filter_pipeline,
                                         [this]() { return ensure_fullscreen_geometry(); },
                                         [this](const char* message) { fail_frame(message); }};
    }

    BgfxLayerCompositeContext composite_context(const ScissorState& scissor_state)
    {
        return BgfxLayerCompositeContext{
            direct_base_requested,
            &root_requires_preservation,
            surface,
            scissor_state,
            render_path,
            &filter_pipeline,
            filter_context(),
            [this](const char* message) { fail_frame(message); },
            [this](const LayerRecord& layer) { return layer_recorded_content_bounds(layer); },
            [this](Rml::LayerHandle handle, std::optional<FbRect> required_bounds) {
                return materialize_layer(handle, required_bounds);
            },
            [this](Rml::LayerHandle handle, const std::vector<size_t>& commands) {
                replay_clip_commands(handle, commands);
            },
            [this](PostprocessTargetKind kind, const FbRect& bounds) {
                return ensure_postprocess_target(kind, bounds);
            },
            [this](const CompositeOp& op) { return composite(op); }};
    }

    // Returns true on success.  Returns false if the source texture is attached to the
    // destination framebuffer (WebGL feedback loop) or other validation fails.
    bool composite(const CompositeOp& op)
    {
        if (frame_failed || !ensure_fullscreen_geometry() || !bgfx::isValid(composite_program) ||
            !bgfx::isValid(op.source.texture))
            return false;

        // WebGL forbids sampling from a texture while rendering into a framebuffer whose
        // color attachment is that same texture (GL_INVALID_OPERATION feedback loop).
        if (bgfx::isValid(op.destination) &&
            texture_attached_to_framebuffer(op.source.texture, op.destination)) {
            fail_frame("composite feedback loop");
            return false;
        }

        const FbRect destination_rect =
            is_empty(op.destination_rect) ? FbRect{0, 0, width, height} : op.destination_rect;
        const LocalFbRect source_rect = op.source.local_rect;
        const bool is_full_frame = (destination_rect.x == 0 && destination_rect.y == 0 &&
                                    destination_rect.w >= width && destination_rect.h >= height);
        auto pass =
            pass_builder.composite(op.destination, destination_rect, op.kind, op.name, op.reason);
        if (!pass)
            return false;
        perf.add_composite(area(destination_rect), is_full_frame);

        CompositeOp submitted_op = op;
        submitted_op.msaa_enabled = framebuffer_msaa_enabled(op.destination);
        return draw_context.submit_composite(*pass, draw_resources(), submitted_op, source_rect,
                                             destination_rect,
                                             stencil_test_state_for_ref(submitted_op.stencil_ref));
    }

    bool copy_region_to_framebuffer(bgfx::TextureHandle source, bgfx::FrameBufferHandle destination,
                                    const Rml::Rectanglei& region, int source_width,
                                    int source_height, const char* name, bool flip_y = false)
    {
        if (!ensure_fullscreen_geometry() || !bgfx::isValid(copy_program) ||
            !bgfx::isValid(source) || !bgfx::isValid(destination))
            return false;
        auto pass = pass_builder.copy(destination, region.Width(), region.Height(), name,
                                      RmlUiPassReason::OtherCopy);
        if (!pass)
            return false;
        perf.add_copy();
        perf.add_copy_pixels(uint64_t(region.Width()) * uint64_t(region.Height()));
        return draw_context.submit_copy(*pass, draw_resources(), source, region, source_width,
                                        source_height, flip_y);
    }

    bgfx::TextureHandle copy_region_to_texture(bgfx::TextureHandle source,
                                               const Rml::Rectanglei& region, int source_width,
                                               int source_height, const char* name,
                                               bool flip_y = false)
    {
        if (region.Width() <= 0 || region.Height() <= 0 || !bgfx::isValid(source))
            return BGFX_INVALID_HANDLE;
        Rml::Rectanglei sample_region = region;
        const bool origin_bottom_left = bgfx::getCaps() && bgfx::getCaps()->originBottomLeft;
        if (flip_y && origin_bottom_left && source_height > region.Height()) {
            const int sample_top = source_height - region.Bottom();
            sample_region = Rml::Rectanglei::FromPositionSize({region.Left(), sample_top},
                                                              {region.Width(), region.Height()});
        }
        const bool can_blit = !flip_y && bgfx::getCaps() &&
                              (bgfx::getCaps()->supported & BGFX_CAPS_TEXTURE_BLIT) != 0;
        const uint64_t flags = (can_blit ? BGFX_TEXTURE_BLIT_DST : BGFX_TEXTURE_RT) |
                               BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
        bgfx::TextureHandle texture =
            bgfx::createTexture2D(uint16_t(region.Width()), uint16_t(region.Height()), false, 1,
                                  bgfx::TextureFormat::RGBA8, flags);
        if (!bgfx::isValid(texture))
            return BGFX_INVALID_HANDLE;

        if (can_blit) {
            auto pass = pass_builder.copy(BGFX_INVALID_HANDLE, region.Width(), region.Height(),
                                          name, copy_pass_reason_from_name(name));
            if (!pass) {
                bgfx::destroy(texture);
                return BGFX_INVALID_HANDLE;
            }
            perf.add_copy();
            perf.add_copy_pixels(uint64_t(region.Width()) * uint64_t(region.Height()));
            draw_context.submit_blit(*pass, texture, source, sample_region);
            return texture;
        }

        bgfx::FrameBufferHandle framebuffer = bgfx::createFrameBuffer(1, &texture, false);
        if (!bgfx::isValid(framebuffer)) {
            bgfx::destroy(texture);
            return BGFX_INVALID_HANDLE;
        }
        const bool copied = copy_region_to_framebuffer(source, framebuffer, sample_region,
                                                       source_width, source_height, name, flip_y);
        bgfx::destroy(framebuffer);
        if (!copied) {
            bgfx::destroy(texture);
            return BGFX_INVALID_HANDLE;
        }
        return texture;
    }

    bgfx::TextureHandle copy_region_to_sized_texture(bgfx::TextureHandle source,
                                                     const Rml::Rectanglei& region,
                                                     int source_width, int source_height,
                                                     Rml::Vector2i output_dimensions,
                                                     Rml::Vector2i destination_offset,
                                                     const char* name, bool flip_y = false)
    {
        if (region.Width() <= 0 || region.Height() <= 0 || output_dimensions.x <= 0 ||
            output_dimensions.y <= 0 || !bgfx::isValid(source)) {
            return BGFX_INVALID_HANDLE;
        }
        if (region.Width() == output_dimensions.x && region.Height() == output_dimensions.y &&
            destination_offset.x == 0 && destination_offset.y == 0) {
            return copy_region_to_texture(source, region, source_width, source_height, name,
                                          flip_y);
        }
        Rml::Rectanglei sample_region = region;
        const bool origin_bottom_left = bgfx::getCaps() && bgfx::getCaps()->originBottomLeft;
        if (flip_y && origin_bottom_left && source_height > region.Height()) {
            const int sample_top = source_height - region.Bottom();
            sample_region = Rml::Rectanglei::FromPositionSize({region.Left(), sample_top},
                                                              {region.Width(), region.Height()});
        }

        constexpr uint64_t flags = BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
        bgfx::TextureHandle texture =
            bgfx::createTexture2D(uint16_t(output_dimensions.x), uint16_t(output_dimensions.y),
                                  false, 1, bgfx::TextureFormat::RGBA8, flags);
        if (!bgfx::isValid(texture)) {
            return BGFX_INVALID_HANDLE;
        }
        bgfx::FrameBufferHandle framebuffer = bgfx::createFrameBuffer(1, &texture, false);
        if (!bgfx::isValid(framebuffer)) {
            bgfx::destroy(texture);
            return BGFX_INVALID_HANDLE;
        }

        auto clear_pass =
            pass_builder.layer_clear(framebuffer, output_dimensions.x, output_dimensions.y);
        if (!clear_pass) {
            bgfx::destroy(framebuffer);
            bgfx::destroy(texture);
            return BGFX_INVALID_HANDLE;
        }
        bgfx::touch(clear_pass->view);

        const int destination_y = flip_y
                                      ? output_dimensions.y - destination_offset.y - region.Height()
                                      : destination_offset.y;
        const LocalFbRect destination_rect{destination_offset.x, destination_y, region.Width(),
                                           region.Height()};
        auto pass = pass_builder.composite(framebuffer, destination_rect, RmlUiPassKind::Copy, name,
                                           copy_pass_reason_from_name(name));
        if (!pass) {
            bgfx::destroy(framebuffer);
            bgfx::destroy(texture);
            return BGFX_INVALID_HANDLE;
        }
        perf.add_copy();
        perf.add_copy_pixels(uint64_t(region.Width()) * uint64_t(region.Height()));
        const bool copied = draw_context.submit_copy(*pass, draw_resources(), source, sample_region,
                                                     source_width, source_height, flip_y);
        bgfx::destroy(framebuffer);
        if (!copied) {
            bgfx::destroy(texture);
            return BGFX_INVALID_HANDLE;
        }
        return texture;
    }

    RenderTargetRecord* ensure_postprocess_target(PostprocessTargetKind kind, const FbRect& bounds)
    {
        return target_cache.acquire_postprocess_target(kind, bounds, surface);
    }

    static uint32_t stencil_test_state_for_ref(uint8_t ref)
    {
        return BGFX_STENCIL_TEST_EQUAL | BGFX_STENCIL_FUNC_REF(uint32_t(ref)) |
               BGFX_STENCIL_FUNC_RMASK(0xff) | BGFX_STENCIL_OP_FAIL_S_KEEP |
               BGFX_STENCIL_OP_FAIL_Z_KEEP | BGFX_STENCIL_OP_PASS_Z_KEEP;
    }

    uint32_t stencil_test_state() const
    {
        const uint8_t ref =
            active_layer < layers.size() ? layers[size_t(active_layer)].stencil_ref : uint8_t(1);
        return stencil_test_state_for_ref(ref);
    }

    uint32_t stencil_replace_state(int value) const
    {
        return BGFX_STENCIL_TEST_ALWAYS | BGFX_STENCIL_FUNC_REF(uint32_t(value)) |
               BGFX_STENCIL_FUNC_RMASK(0xff) | BGFX_STENCIL_OP_FAIL_S_KEEP |
               BGFX_STENCIL_OP_FAIL_Z_KEEP | BGFX_STENCIL_OP_PASS_Z_REPLACE;
    }

    void clear_active_stencil(uint8_t value, const ScissorState& scissor,
                              std::optional<FbRect> global_clear_bounds = std::nullopt)
    {
        LayerRecord* layer = current_layer();
        if (!layer)
            return;
        FbRect clear_bounds = clip_work_bounds(layer, scissor);
        if (global_clear_bounds) {
            clear_bounds =
                intersect(clear_bounds, local_rect_for_layer(*global_clear_bounds, *layer));
        }
        if (is_empty(clear_bounds))
            return;
        const bool is_full = is_full_frame_rect(clear_bounds, width, height);
        auto pass = pass_builder.stencil_clear(layer->framebuffer, clear_bounds, value);
        if (!pass)
            return;
        perf.add_clear(uint64_t(clear_bounds.w) * uint64_t(clear_bounds.h), is_full);
        bgfx::touch(pass->view);
    }

    uint32_t stencil_intersect_state(uint8_t previous_ref) const
    {
        return BGFX_STENCIL_TEST_EQUAL | BGFX_STENCIL_FUNC_REF(uint32_t(previous_ref)) |
               BGFX_STENCIL_FUNC_RMASK(0xff) | BGFX_STENCIL_OP_FAIL_S_KEEP |
               BGFX_STENCIL_OP_FAIL_Z_KEEP | BGFX_STENCIL_OP_PASS_Z_INCR;
    }

    uint32_t stencil_decrement_state(uint8_t ref) const
    {
        return BGFX_STENCIL_TEST_EQUAL | BGFX_STENCIL_FUNC_REF(uint32_t(ref)) |
               BGFX_STENCIL_FUNC_RMASK(0xff) | BGFX_STENCIL_OP_FAIL_S_KEEP |
               BGFX_STENCIL_OP_FAIL_Z_KEEP | BGFX_STENCIL_OP_PASS_Z_DECR;
    }

    bool decrement_stencil_ref(uint8_t ref)
    {
        if (ref <= 1)
            return true;
        if (frame_failed || !ensure_fullscreen_geometry() || !bgfx::isValid(composite_program) ||
            !bgfx::isValid(white_texture))
            return false;
        LayerRecord* layer = current_layer();
        if (!layer)
            return false;
        const FbRect work_bounds =
            clip_work_bounds(layer, ScissorState{scissor_enabled, scissor_region});
        if (is_empty(work_bounds))
            return true;
        auto pass =
            pass_builder.geometry(layer->framebuffer, work_bounds.w, work_bounds.h,
                                  "RmlUi.StencilNormalize", RmlUiPassReason::StencilNormalize);
        if (!pass)
            return false;

        return draw_context.submit_stencil_decrement(*pass, draw_resources(),
                                                     stencil_decrement_state(ref));
    }

    bool normalize_active_stencil_to_one()
    {
        LayerRecord* layer = current_layer();
        if (!layer)
            return false;
        for (uint8_t ref = layer->stencil_ref; ref > 1; --ref) {
            if (!decrement_stencil_ref(ref))
                return false;
        }
        layer->stencil_ref = 1;
        return true;
    }

    void submit_to_clip_mask(const GeometryRecord& geometry, Rml::Vector2f translation,
                             uint32_t stencil_state, const ScissorState& scissor,
                             bool command_transform_valid,
                             const std::array<float, 16>& command_transform)
    {
        if (frame_failed || !bgfx::isValid(program) || geometry.index_count == 0 ||
            pass_builder.exhausted())
            return;
        LayerRecord* layer = current_layer();
        if (!layer)
            return;
        const FbRect work_bounds = clip_work_bounds(layer, scissor);
        if (is_empty(work_bounds))
            return;
        auto pass =
            pass_builder.geometry(layer->framebuffer, layer->texture_width, layer->texture_height,
                                  "RmlUi.ClipMask", RmlUiPassReason::ClipMask);
        if (!pass)
            return;
        perf.add_clip_mask(uint64_t(work_bounds.w) * uint64_t(work_bounds.h));

        draw_context.submit_clip_mask(
            *pass, draw_resources(), geometry, *layer,
            BgfxClipMaskDrawState{translation, scissor, command_transform_valid,
                                  command_transform.data(), layer->msaa_enabled, stencil_state});
    }

    void apply_clip_command(const ClipCommand& command, bool record_on_layer)
    {
        auto it = geometries.find(command.geometry);
        if (it == geometries.end())
            return;
        std::optional<FbRect> command_bounds =
            command_fb_bounds(command.geometry, command.translation, command.scissor,
                              command.transform_valid, command.transform);
        switch (command.operation) {
        case Rml::ClipMaskOperation::Set:
            // Set replaces the active clip region within the current scissor/save bounds. Do not
            // tighten this clear to the Set geometry bounds: inset box-shadow clipping commonly
            // follows a SetInverse operation, and stale stencil pixels outside the following Set
            // geometry can let blurred inset pixels leak past rounded corners.
            clear_active_stencil(0, command.scissor);
            submit_to_clip_mask(it->second, command.translation, stencil_replace_state(1),
                                command.scissor, command.transform_valid, command.transform);
            break;
        case Rml::ClipMaskOperation::SetInverse:
            clear_active_stencil(1, command.scissor);
            submit_to_clip_mask(it->second, command.translation, stencil_replace_state(0),
                                command.scissor, command.transform_valid, command.transform);
            break;
        case Rml::ClipMaskOperation::Intersect:
            if (LayerRecord* layer = current_layer();
                layer && layer->stencil_ref == 254 && command.previous_ref == 1) {
                if (!normalize_active_stencil_to_one()) {
                    fail_frame(
                        "failed to normalize stencil clip mask before overflow intersection");
                    return;
                }
            }
            submit_to_clip_mask(it->second, command.translation,
                                stencil_intersect_state(command.previous_ref), command.scissor,
                                command.transform_valid, command.transform);
            break;
        }
        if (LayerRecord* layer = current_layer()) {
            layer->stencil_ref = command.next_ref;
            if (record_on_layer) {
                FbRect fallback_bounds = layer_limit_bounds(*layer);
                if (command.scissor.enabled) {
                    const FbRect scissor_bounds = scissor_fb_bounds(command.scissor);
                    if (!is_empty(scissor_bounds))
                        fallback_bounds = intersect(fallback_bounds, scissor_bounds);
                }
                if (command_bounds) {
                    layer->conservative_mask_bounds = update_conservative_mask_bounds(
                        layer->conservative_mask_bounds, command.operation, *command_bounds,
                        fallback_bounds);
                }
                const FbRect recorded_bounds =
                    command.scissor.enabled
                        ? active_scissor_bounds(command.scissor, layer->bounds.framebuffer)
                        : full_local_rect(*layer);
                if (!is_empty(recorded_bounds)) {
                    layer->clip_commands.push_back(clip_commands.size());
                    clip_commands.push_back(command);
                }
            }
        }
    }

    void replay_clip_commands(Rml::LayerHandle layer_handle, const std::vector<size_t>& commands)
    {
        const Rml::LayerHandle saved_active = active_layer;
        const bool saved_scissor_enabled = scissor_enabled;
        const Rml::Rectanglei saved_scissor_region = scissor_region;
        active_layer = layer_handle;
        LayerRecord* layer = current_layer();
        for (size_t index : commands) {
            if (index < clip_commands.size()) {
                const ClipCommand& command = clip_commands[index];
                if (!layer)
                    continue;
                const FbRect command_bounds =
                    command.scissor.enabled
                        ? active_scissor_bounds(command.scissor, layer->bounds.framebuffer)
                        : layer->bounds.framebuffer;
                if (is_empty(command_bounds))
                    continue;
                apply_clip_command(command, false);
            }
        }
        active_layer = saved_active;
        scissor_enabled = saved_scissor_enabled;
        scissor_region = saved_scissor_region;
    }

    void submit_gradient(const ShaderRecord& shader, const GeometryRecord& geometry,
                         Rml::Vector2f translation)
    {
        if (frame_failed || !bgfx::isValid(gradient_program) || geometry.index_count == 0 ||
            pass_builder.exhausted())
            return;
        LayerRecord* layer = current_layer();
        if (!layer || shader.kind != ShaderRecordKind::Gradient ||
            shader.gradient.kind == GradientKind::Invalid || shader.gradient.stop_count == 0)
            return;
        const int lw = layer->texture_width;
        const int lh = layer->texture_height;
        auto pass = pass_builder.geometry(layer->framebuffer, lw, lh, "RmlUi.Gradient",
                                          RmlUiPassReason::Gradient);
        if (!pass)
            return;
        perf.add_gradient();

        draw_context.submit_gradient(
            *pass, draw_resources(), shader, geometry, *layer,
            BgfxGradientDrawState{translation, ScissorState{scissor_enabled, scissor_region},
                                  transform_valid, transform, layer->clip_mask_enabled,
                                  layer->msaa_enabled, stencil_test_state()});
        add_submitted_region(*layer, geometry, translation);
    }

    void submit_material_shader(const ShaderRecord& shader, const GeometryRecord& geometry,
                                Rml::Vector2f translation, Rml::TextureHandle texture)
    {
        if (frame_failed || !material_shader_provider ||
            shader.kind != ShaderRecordKind::Material || !shader.material.valid() ||
            geometry.index_count == 0 || pass_builder.exhausted())
            return;
        LayerRecord* layer = current_layer();
        if (!layer)
            return;
        const int lw = layer->texture_width;
        const int lh = layer->texture_height;
        auto pass = pass_builder.geometry(layer->framebuffer, lw, lh, "RmlUi.MaterialShader",
                                          RmlUiPassReason::OrdinaryGeometry);
        if (!pass)
            return;

        bgfx::TextureHandle bgfx_texture = white_texture;
        int texture_width = 1;
        int texture_height = 1;
        if (auto it = textures.find(texture);
            it != textures.end() && bgfx::isValid(it->second.handle)) {
            bgfx_texture = it->second.handle;
            texture_width = it->second.dimensions.x;
            texture_height = it->second.dimensions.y;
        }

        Rml::Rectanglei local_scissor = Rml::Rectanglei::FromPositionSize({0, 0}, {0, 0});
        if (scissor_enabled) {
            local_scissor = clamp_scissor_local(scissor_region, layer->bounds.framebuffer);
            if (local_scissor.Width() <= 0 || local_scissor.Height() <= 0)
                return;
        }

        RmlUiMaterialShaderDrawContext context;
        context.view = pass->view;
        context.vertex_buffer = geometry.vb;
        context.index_buffer = geometry.ib;
        context.index_count = geometry.index_count;
        context.projection = layer->projection;
        context.transform = transform_valid ? transform : identity;
        context.translation = translation;
        context.scissor_enabled = scissor_enabled;
        context.local_scissor = local_scissor;
        context.clip_mask_enabled = layer->clip_mask_enabled;
        context.msaa_enabled = layer->msaa_enabled;
        context.stencil_state = stencil_test_state();
        context.texture = bgfx_texture;
        context.texture_width = texture_width;
        context.texture_height = texture_height;
        context.paint_dimensions = shader.paint_dimensions;
        context.dpi_scale = surface.scale_x;
        context.projection_uniform = projection_uniform;
        context.transform_uniform = transform_uniform;
        context.translate_uniform = translate_uniform;
        context.white_texture = white_texture;
        context.premultiplied_blend_state = kRmlBlendState;

        if (material_shader_provider->submit_decorator_shader(shader.material, context)) {
            perf.add_geometry(uint64_t(lw) * uint64_t(lh), geometry.index_count);
            add_submitted_region(*layer, geometry, translation);
        }
    }

    void submit_shader(const ShaderRecord& shader, const GeometryRecord& geometry,
                       Rml::Vector2f translation, Rml::TextureHandle texture)
    {
        switch (shader.kind) {
        case ShaderRecordKind::Gradient:
            submit_gradient(shader, geometry, translation);
            break;
        case ShaderRecordKind::Material:
            submit_material_shader(shader, geometry, translation, texture);
            break;
        case ShaderRecordKind::Invalid:
            break;
        }
    }

    ShaderProvider* shader_provider = nullptr;
    TextureLoader* textures_provider = nullptr;
    Diagnostics* diagnostics = nullptr;
    MaterialShaderProvider* material_shader_provider = nullptr;
    PerfLogger* perf_logger = nullptr;
    bgfx::VertexLayout layout;
    bgfx::VertexLayout fullscreen_layout;
    bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle composite_program = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle composite_filter_program = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle copy_program = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle opacity_program = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle color_matrix_program = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle mask_multiply_program = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle blur_program = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle drop_shadow_program = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle gradient_program = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle white_texture = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle sampler = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle mask_sampler = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle projection_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle transform_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle translate_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle color_matrix_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle opacity_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle gradient_params_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle gradient_stops_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle gradient_stop_meta_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle blur_params_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle blur_weights_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle texcoord_bounds_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle mask_texcoord_transform_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle shadow_color_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle shadow_offset_uniform = BGFX_INVALID_HANDLE;
    std::unordered_map<Rml::CompiledGeometryHandle, GeometryRecord> geometries;
    std::unordered_set<Rml::CompiledGeometryHandle> deferred_geometry_release;
    std::unordered_map<Rml::TextureHandle, TextureRecord> textures;
    std::unordered_map<Rml::CompiledFilterHandle, FilterRecord> filters;
    std::unordered_map<Rml::CompiledShaderHandle, ShaderRecord> shaders;
    std::vector<ClipCommand> clip_commands;
    Rml::CompiledGeometryHandle geometry_counter = 0;
    bgfx::VertexBufferHandle fullscreen_vb = BGFX_INVALID_HANDLE;
    Rml::TextureHandle texture_counter = 0;
    Rml::CompiledFilterHandle filter_counter = 0;
    Rml::CompiledShaderHandle shader_counter = 0;
    int width = 1;
    int height = 1;
    int logical_width = 1;
    int logical_height = 1;
    SurfaceMetrics surface{};
    float projection[16]{};
    float identity[16]{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float transform[16]{};
    bool transform_valid = false;
    bool scissor_enabled = false;
    Rml::Rectanglei scissor_region = Rml::Rectanglei::FromPositionSize({0, 0}, {0, 0});
    bool frame_failed = false;
    bool direct_base_requested = false;
    bool direct_base_presented = false;
    bool root_requires_preservation = false;
    const char* direct_base_fallback_reason = nullptr;
    const char* logged_base_fallback_reason = nullptr;
    bool base_direct_compatibility_enabled = false;
    RenderPath render_path = RenderPath::Optimized;
    BlurSampleBoundsMode blur_sample_bounds_mode = BlurSampleBoundsMode::SourceBounds;
    uint8_t reference_msaa_samples = 2;
    bool trace_filter_pipeline = false;

    // Cached stencil format (probed once to avoid getInternalformatParameter spam).
    mutable bool stencil_cached = false;
    mutable bgfx::TextureFormat::Enum cached_stencil_format = bgfx::TextureFormat::Unknown;

    rmlui_bgfx::PerfCounters perf;
    BgfxDrawContext draw_context;
    BgfxFilterPipeline filter_pipeline;
    BgfxReferenceRenderer reference_renderer;
    BgfxPassBuilder pass_builder;
    BgfxTargetCache target_cache{&perf};
    BgfxLayerSystem layer_system{target_cache};
    std::vector<LayerRecord>& layers = target_cache.layers();
    std::deque<RenderTargetRecord>& postprocess_targets = target_cache.postprocess_targets();
    LayerPoolPlan& layer_pool = target_cache.layer_pool();
    PostprocessPoolPlan& postprocess_pool = target_cache.postprocess_pool();
    std::vector<Rml::LayerHandle>& layer_stack = layer_system.stack();
    Rml::LayerHandle& active_layer = layer_system.active_layer_ref();
    bool perf_logging_enabled = false;

    // Check whether a texture is the color attachment of a framebuffer we own.
    // WebGL forbids sampling a texture while rendering into a framebuffer whose
    // color attachment is that same texture (GL_INVALID_OPERATION feedback loop).
    bool framebuffer_msaa_enabled(bgfx::FrameBufferHandle framebuffer) const
    {
        if (!bgfx::isValid(framebuffer)) {
            return false;
        }
        for (const LayerRecord& layer : layers) {
            if (bgfx::isValid(layer.framebuffer) && layer.framebuffer.idx == framebuffer.idx) {
                return layer.msaa_enabled;
            }
        }
        return false;
    }

    bool texture_attached_to_framebuffer(bgfx::TextureHandle texture,
                                         bgfx::FrameBufferHandle framebuffer) const
    {
        for (const RenderTargetRecord& target : postprocess_targets) {
            if (bgfx::isValid(target.framebuffer) && target.framebuffer.idx == framebuffer.idx &&
                bgfx::isValid(target.color) && target.color.idx == texture.idx) {
                return true;
            }
        }
        for (const LayerRecord& layer : layers) {
            if (bgfx::isValid(layer.framebuffer) && layer.framebuffer.idx == framebuffer.idx &&
                bgfx::isValid(layer.color) && layer.color.idx == texture.idx) {
                return true;
            }
        }
        return false;
    }
};

RenderInterface::RenderInterface(RendererConfig config) : m_impl(std::make_unique<Impl>(config)) {}

RenderInterface::~RenderInterface() = default;

RenderInterface::operator bool() const
{
    return m_impl && bgfx::isValid(m_impl->program) && bgfx::isValid(m_impl->composite_program) &&
           bgfx::isValid(m_impl->composite_filter_program) && bgfx::isValid(m_impl->copy_program) &&
           bgfx::isValid(m_impl->opacity_program) && bgfx::isValid(m_impl->color_matrix_program) &&
           bgfx::isValid(m_impl->mask_multiply_program) && bgfx::isValid(m_impl->blur_program) &&
           bgfx::isValid(m_impl->drop_shadow_program) && bgfx::isValid(m_impl->gradient_program);
}

void RenderInterface::resize(const SurfaceMetrics& surface) { m_impl->resize(surface); }

void RenderInterface::begin_frame()
{
    m_impl->pass_builder.begin_frame(m_impl->width, m_impl->height);
    m_impl->transform_valid = false;
    m_impl->scissor_enabled = false;
    m_impl->scissor_region =
        Rml::Rectanglei::FromPositionSize({0, 0}, {m_impl->width, m_impl->height});

    if (m_impl->render_path == RenderPath::Reference) {
        m_impl->release_deferred_geometries();
        m_impl->perf.reset();
        m_impl->frame_failed = false;
        m_impl->direct_base_presented = false;
        m_impl->direct_base_fallback_reason = nullptr;
        m_impl->reference_renderer.set_context(m_impl->reference_context());
        m_impl->reference_renderer.begin_frame(m_impl->surface, m_impl->depth_stencil_format());
        return;
    }

    if (!m_impl->begin_base_layer())
        return;
    LayerRecord* base = m_impl->current_layer();
    if (!base)
        return;
    auto pass = m_impl->pass_builder.base_clear(base->framebuffer, m_impl->width, m_impl->height);
    if (pass) {
        m_impl->perf.add_clear(uint64_t(m_impl->width) * uint64_t(m_impl->height), true);
        bgfx::touch(pass->view);
    }
}

void RenderInterface::end_frame()
{
    if (m_impl->render_path == RenderPath::Reference) {
        m_impl->reference_renderer.end_frame();
    } else {
        if (m_impl->frame_failed) {
            m_impl->layer_system.begin_frame();
            return;
        }
        if (m_impl->layer_stack.size() != 1) {
            std::fprintf(stderr, "[rmlui] unbalanced layer stack at frame end: %zu\n",
                         m_impl->layer_stack.size());
            m_impl->layer_system.begin_frame();
        }
        if (!m_impl->direct_base_requested) {
            auto clear_pass =
                m_impl->pass_builder.base_clear(BGFX_INVALID_HANDLE, m_impl->width, m_impl->height);
            if (clear_pass) {
                m_impl->perf.add_clear(uint64_t(m_impl->width) * uint64_t(m_impl->height), true);
                bgfx::touch(clear_pass->view);
            } else {
                m_impl->fail_frame("end_frame backbuffer clear failed");
                return;
            }
            if (LayerRecord* base = m_impl->layer_for_handle(0)) {
                if (!m_impl->composite(make_composite_op(
                        texture_region(base->color, base->bounds.framebuffer,
                                       full_local_rect(*base), base->texture_width,
                                       base->texture_height),
                        BGFX_INVALID_HANDLE, Rml::BlendMode::Blend, ScissorState{false, {}}, false,
                        1, RmlUiPassKind::FinalComposite, RmlUiPassReason::FinalComposite,
                        "RmlUi.FinalComposite",
                        LocalFbRect{0, 0, m_impl->width, m_impl->height}))) {
                    m_impl->fail_frame("end_frame final composite failed");
                }
            }
        } else {
            m_impl->direct_base_presented = true;
            if (m_impl->direct_base_fallback_reason) {
                std::fprintf(stderr, "[rmlui] direct base presentation: %s\n",
                             m_impl->direct_base_fallback_reason);
            }
        }
    }

    // Per-frame performance logging (~1 Hz, gated by runtime flag).
    {
#ifdef RMLUI_BGFX_ENABLE_RENDER_PERF
        static double last_log_time = 0.0;
        static int frame_count = 0;
        frame_count++;
        const int64_t now_ticks = bx::getHPCounter();
        const double now = double(now_ticks) / double(bx::getHPFrequency());
        if (m_impl->perf_logging_enabled && now - last_log_time >= 1.0) {
            const auto& p = m_impl->perf;
            char line[4096];
            std::snprintf(
                line, sizeof(line),
                "[perf] fps=%.0f passes=%u views=%u view_reuses=%u geom=%u clip=%u "
                "gradients=%u pass_geom=%u pass_gradient=%u pass_clip=%u "
                "pass_stencil_norm=%u pass_base_clear=%u pass_layer_clear=%u "
                "pass_stencil_clear=%u pass_filter_copy=%u pass_filter_opacity=%u "
                "pass_filter_color=%u pass_filter_mask=%u pass_filter_blur=%u "
                "pass_filter_shadow=%u pass_filter_shadow_comp=%u color_filter_folds=%u "
                "pass_layer_scratch=%u pass_layer_comp=%u pass_final_comp=%u "
                "pass_save_texture=%u pass_save_mask=%u pass_other_copy=%u pass_other=%u "
                "layers=%u full_layers=%u bounded_layers=%u "
                "full_frame_child_layers=%u bounded_child_layers=%u "
                "unbounded_layer_fallbacks=%u "
                "unbounded_no_scissor_fallbacks=%u "
                "unbounded_transform_fallbacks=%u "
                "unbounded_inverse_clip_fallbacks=%u "
                "filters=%u blur=%u shadow=%u mask=%u "
                "base_direct=%u base_offscreen=%u base_fallback=%u "
                "clear_px=%llu copy_px=%llu composite_px=%llu post_px=%llu "
                "full_frame_passes=%u bounded_passes=%u "
                "full_frame_clear_passes=%u bounded_clear_passes=%u "
                "full_frame_composite_passes=%u bounded_composite_passes=%u "
                "full_frame_postprocess_passes=%u bounded_postprocess_passes=%u "
                "full_frame_postprocess_target_uses=%u bounded_postprocess_target_uses=%u "
                "full_frame_postprocess_targets=%u bounded_postprocess_targets=%u "
                "rt_alloc=%u rt_destroy=%u layer_alloc=%u layer_destroy=%u "
                "max_layer=%ux%u max_child_layer=%ux%u max_child_rt=%ux%u "
                "max_rt=%ux%u fb=%dx%d",
                double(frame_count) / (now - last_log_time), p.pass_count, p.view_count,
                p.view_reuses, p.geometry_draws, p.clip_mask_draws, p.gradient_draws,
                p.ordinary_geometry_passes, p.gradient_passes, p.clip_mask_passes,
                p.stencil_normalize_passes, p.base_clear_passes, p.layer_clear_passes,
                p.stencil_clear_passes, p.filter_copy_passes, p.filter_opacity_passes,
                p.filter_color_matrix_passes, p.filter_mask_image_passes, p.filter_blur_passes,
                p.filter_drop_shadow_passes, p.filter_drop_shadow_composite_passes,
                p.color_filter_composite_folds, p.layer_scratch_copy_passes,
                p.layer_composite_reason_passes, p.final_composite_passes,
                p.save_texture_copy_passes, p.save_mask_copy_passes, p.other_copy_passes,
                p.other_passes, p.layer_pushes, p.full_frame_layers, p.bounded_layers,
                p.full_frame_child_layers, p.bounded_child_layers, p.unbounded_layer_fallbacks,
                p.unbounded_no_scissor_fallbacks, p.unbounded_transform_fallbacks,
                p.unbounded_inverse_clip_fallbacks, p.postprocess_passes, p.blur_passes,
                p.dropshadow_passes, p.mask_passes, p.direct_base_presentations,
                p.offscreen_base_presentations, p.direct_base_fallbacks,
                (unsigned long long)p.clear_pixels, (unsigned long long)p.copy_pixels,
                (unsigned long long)p.composite_pixels, (unsigned long long)p.postprocess_pixels,
                p.full_frame_passes, p.bounded_passes, p.full_frame_clear_passes,
                p.bounded_clear_passes, p.full_frame_composite_passes, p.bounded_composite_passes,
                p.full_frame_postprocess_passes, p.bounded_postprocess_passes,
                p.full_frame_postprocess_target_uses, p.bounded_postprocess_target_uses,
                p.full_frame_postprocess_targets, p.bounded_postprocess_targets,
                p.postprocess_allocations, p.postprocess_destroys, p.layer_allocations,
                p.layer_destroys, p.max_layer_width, p.max_layer_height, p.max_child_layer_width,
                p.max_child_layer_height, p.max_child_layer_width, p.max_child_layer_height,
                p.max_postprocess_width, p.max_postprocess_height, m_impl->width, m_impl->height);
            m_impl->log_perf_line(line);
            frame_count = 0;
            last_log_time = now;
        }
#endif
    }
}

Rml::CompiledGeometryHandle RenderInterface::CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                             Rml::Span<const int> indices)
{
    if (vertices.size() > std::numeric_limits<uint32_t>::max() ||
        indices.size() > std::numeric_limits<uint32_t>::max()) {
        return 0;
    }
    const GeometryBoundsResult local_bounds = compute_indexed_geometry_bounds(vertices, indices);
    if (local_bounds.status == GeometryBoundsStatus::EmptyGeometry) {
        const Rml::CompiledGeometryHandle handle = ++m_impl->geometry_counter;
        m_impl->geometries.emplace(handle, GeometryRecord{});
        return handle;
    }
    if (local_bounds.status != GeometryBoundsStatus::Valid) {
        return 0;
    }

    std::vector<RmlVertex> converted;
    converted.reserve(vertices.size());
    for (const Rml::Vertex& vertex : vertices) {
        converted.push_back({vertex.position.x, vertex.position.y, pack_abgr(vertex.colour),
                             vertex.tex_coord.x, vertex.tex_coord.y});
    }
    std::vector<uint32_t> converted_indices;
    converted_indices.reserve(indices.size());
    for (int index : indices) {
        converted_indices.push_back(uint32_t(index));
    }
    auto vb = bgfx::createVertexBuffer(
        bgfx::copy(converted.data(), uint32_t(converted.size() * sizeof(RmlVertex))),
        m_impl->layout);
    auto ib = bgfx::createIndexBuffer(
        bgfx::copy(converted_indices.data(), uint32_t(converted_indices.size() * sizeof(uint32_t))),
        BGFX_BUFFER_INDEX32);
    if (!bgfx::isValid(vb) || !bgfx::isValid(ib)) {
        if (bgfx::isValid(vb))
            bgfx::destroy(vb);
        if (bgfx::isValid(ib))
            bgfx::destroy(ib);
        return 0;
    }
    const Rml::CompiledGeometryHandle handle = ++m_impl->geometry_counter;
    m_impl->geometries.emplace(
        handle, GeometryRecord{vb, ib, uint32_t(converted_indices.size()), local_bounds.logical});
    return handle;
}

void RenderInterface::RenderGeometry(Rml::CompiledGeometryHandle geometry,
                                     Rml::Vector2f translation, Rml::TextureHandle texture)
{
    auto it = m_impl->geometries.find(geometry);
    if (it == m_impl->geometries.end())
        return;
    if (m_impl->render_path == RenderPath::Reference) {
        m_impl->reference_renderer.render_geometry(geometry, translation, texture);
        return;
    }
    if (m_impl->active_layer_is_recording()) {
        m_impl->record_geometry_command(geometry, translation, texture);
        return;
    }
    m_impl->submit(it->second, translation, texture);
}

void RenderInterface::ReleaseGeometry(Rml::CompiledGeometryHandle geometry)
{
    if (auto it = m_impl->geometries.find(geometry); it != m_impl->geometries.end()) {
        if (m_impl->recorded_geometry_reference_exists(geometry) ||
            m_impl->reference_renderer.geometry_in_use(geometry)) {
            m_impl->deferred_geometry_release.insert(geometry);
            return;
        }
        Impl::destroy_geometry(it->second);
        m_impl->geometries.erase(it);
    }
}

Rml::TextureHandle RenderInterface::LoadTexture(Rml::Vector2i& texture_dimensions,
                                                const Rml::String& source)
{
    texture_dimensions = {};
    if (!m_impl->textures_provider) {
        m_impl->log_error("texture loader is not configured");
        return 0;
    }

    LoadedTexture loaded;
    std::string error_message;
    if (!m_impl->textures_provider->load_rgba8(source.c_str(), loaded, &error_message) ||
        loaded.width <= 0 || loaded.height <= 0 ||
        loaded.rgba8.size() != size_t(loaded.width) * size_t(loaded.height) * 4u) {
        std::string message = "texture load failed: ";
        message += source.c_str();
        if (!error_message.empty()) {
            message += " (";
            message += error_message;
            message += ")";
        }
        m_impl->log_error(message);
        return 0;
    }

    const int width = loaded.width;
    const int height = loaded.height;
    Rml::TextureHandle handle =
        m_impl->create_texture_from_rgba(std::move(loaded.rgba8), width, height, false);
    if (handle != 0) {
        texture_dimensions = {width, height};
    }
    return handle;
}

Rml::TextureHandle RenderInterface::GenerateTexture(Rml::Span<const Rml::byte> source,
                                                    Rml::Vector2i source_dimensions)
{
    if (source.empty()) {
        return 0;
    }
    std::vector<uint8_t> rgba(source.begin(), source.end());
    return m_impl->create_texture_from_rgba(std::move(rgba), source_dimensions.x,
                                            source_dimensions.y, true);
}

void RenderInterface::ReleaseTexture(Rml::TextureHandle texture)
{
    if (auto it = m_impl->textures.find(texture); it != m_impl->textures.end()) {
        if (texture_ownership_releases_handle(it->second.ownership) &&
            bgfx::isValid(it->second.handle)) {
            bgfx::destroy(it->second.handle);
        }
        m_impl->textures.erase(it);
    }
}

void RenderInterface::EnableScissorRegion(bool enable)
{
    m_impl->scissor_enabled = enable;
    if (m_impl->render_path == RenderPath::Reference) {
        m_impl->reference_renderer.enable_scissor_region(enable);
    }
}

void RenderInterface::SetScissorRegion(Rml::Rectanglei region)
{
    m_impl->scissor_region = clamp_scissor(logical_scissor_to_framebuffer(region, m_impl->surface),
                                           m_impl->width, m_impl->height);
    if (m_impl->render_path == RenderPath::Reference) {
        m_impl->reference_renderer.set_scissor_region(m_impl->scissor_region);
    }
}

void RenderInterface::SetTransform(const Rml::Matrix4f* transform)
{
    if (transform) {
        m_impl->transform_valid = true;
        std::memcpy(m_impl->transform, transform->data(), sizeof(m_impl->transform));
    } else {
        m_impl->transform_valid = false;
    }
    if (m_impl->render_path == RenderPath::Reference) {
        m_impl->reference_renderer.set_transform(m_impl->transform_valid ? m_impl->transform
                                                                         : nullptr);
    }
}

void RenderInterface::EnableClipMask(bool enable)
{
    if (m_impl->render_path == RenderPath::Reference) {
        m_impl->reference_renderer.enable_clip_mask(enable);
        return;
    }
    if (LayerRecord* layer = m_impl->current_layer()) {
        layer->clip_mask_enabled = enable;
    }
}

void RenderInterface::RenderToClipMask(Rml::ClipMaskOperation operation,
                                       Rml::CompiledGeometryHandle geometry,
                                       Rml::Vector2f translation)
{
    if (m_impl->render_path == RenderPath::Reference) {
        m_impl->reference_renderer.render_to_clip_mask(operation, geometry, translation);
        return;
    }
    auto geometry_it = m_impl->geometries.find(geometry);
    if (geometry_it == m_impl->geometries.end() || geometry_it->second.index_count == 0)
        return;
    LayerRecord* layer = m_impl->current_layer();
    if (!layer)
        return;

    ClipCommand command;
    command.operation = operation;
    command.geometry = geometry;
    command.translation = translation;
    command.scissor = ScissorState{m_impl->scissor_enabled, m_impl->scissor_region};
    command.transform_valid = m_impl->transform_valid;
    if (command.transform_valid) {
        std::memcpy(command.transform.data(), m_impl->transform, sizeof(m_impl->transform));
    }
    const StencilClipPlan clip_plan =
        plan_stencil_clip_operation(layer->stencil_ref, clip_operation_plan(operation));
    command.previous_ref = clip_plan.previous_ref;
    command.next_ref = clip_plan.next_ref;
    if (m_impl->active_layer_is_recording()) {
        m_impl->record_clip_mask_command(command);
        return;
    }
    m_impl->apply_clip_command(command, true);
}

Rml::LayerHandle RenderInterface::PushLayer()
{
    if (m_impl->render_path == RenderPath::Reference) {
        return m_impl->reference_renderer.push_layer();
    }
    m_impl->perf.add_layer_push();
    const Rml::LayerHandle parent = m_impl->active_layer;
    const Rml::LayerHandle handle = Rml::LayerHandle(m_impl->layer_pool.push());
    if (uint32_t(handle) == LayerPoolPlan::InvalidLayer)
        return handle;
    const ScissorState push_scissor{m_impl->scissor_enabled, m_impl->scissor_region};
    const bool push_transform_valid = m_impl->transform_valid;
    // Child layers are virtual at push time, so this is only a conservative containment fallback
    // for inherited clips, saved masks, and empty-layer materialization. Final child target bounds
    // come from recorded content using the captured per-command transforms.
    const RenderBounds provisional_bounds =
        m_impl->compute_child_layer_bounds(parent, push_scissor, push_transform_valid, false);
    m_impl->layer_system.prepare_virtual_child(handle, parent, provisional_bounds, push_scissor,
                                               push_transform_valid);
    m_impl->layer_system.push_layer(handle);
    return handle;
}

void RenderInterface::CompositeLayers(Rml::LayerHandle source, Rml::LayerHandle destination,
                                      Rml::BlendMode blend_mode,
                                      Rml::Span<const Rml::CompiledFilterHandle> filters)
{
    if (m_impl->render_path == RenderPath::Reference) {
        m_impl->reference_renderer.composite_layers(source, destination, blend_mode, filters);
        return;
    }
    const ScissorState scissor_state{m_impl->scissor_enabled, m_impl->scissor_region};
    m_impl->layer_system.composite_layers(m_impl->composite_context(scissor_state), source,
                                          destination, blend_mode, filters);
}

void RenderInterface::PopLayer()
{
    if (m_impl->render_path == RenderPath::Reference) {
        m_impl->reference_renderer.pop_layer();
        return;
    }
    if (m_impl->layer_stack.size() <= 1) {
        std::fprintf(stderr, "[rmlui] attempted to pop the base RmlUi layer\n");
        return;
    }
    m_impl->layer_system.pop_layer();
}

Rml::TextureHandle RenderInterface::SaveLayerAsTexture()
{
    if (m_impl->render_path == RenderPath::Reference) {
        return m_impl->reference_renderer.save_layer_as_texture();
    }
    if (m_impl->frame_failed)
        return 0;
    return m_impl->layer_system.save_layer_as_texture(m_impl->save_texture_context());
}

Rml::CompiledFilterHandle RenderInterface::SaveLayerAsMaskImage()
{
    if (m_impl->render_path == RenderPath::Reference) {
        return m_impl->reference_renderer.save_layer_as_mask_image();
    }
    if (m_impl->frame_failed)
        return 0;
    return m_impl->layer_system.save_layer_as_mask_image(m_impl->save_mask_context());
}

Rml::CompiledFilterHandle RenderInterface::CompileFilter(const Rml::String& name,
                                                         const Rml::Dictionary& parameters)
{
    FilterRecord filter;
    if (name == "opacity") {
        filter = make_opacity_filter(Rml::Get(parameters, "value", 1.0f));
    } else if (name == "blur") {
        filter.kind = FilterKind::Blur;
        filter.sigma = Rml::Get(parameters, "sigma", 1.0f);
    } else if (name == "drop-shadow") {
        filter.kind = FilterKind::DropShadow;
        filter.sigma = Rml::Get(parameters, "sigma", 0.0f);
        const Rml::Vector2f offset = Rml::Get(parameters, "offset", Rml::Vector2f(0.0f));
        const Rml::ColourbPremultiplied color =
            Rml::Get(parameters, "color", Rml::Colourb()).ToPremultiplied();
        filter.offset = {offset.x, offset.y};
        filter.color = {float(color.red) / 255.0f, float(color.green) / 255.0f,
                        float(color.blue) / 255.0f, float(color.alpha) / 255.0f};
    } else if (name == "brightness") {
        filter = make_brightness_filter(Rml::Get(parameters, "value", 1.0f));
    } else if (name == "contrast") {
        filter = make_contrast_filter(Rml::Get(parameters, "value", 1.0f));
    } else if (name == "invert") {
        filter = make_invert_filter(Rml::Get(parameters, "value", 1.0f));
    } else if (name == "grayscale") {
        filter = make_grayscale_filter(Rml::Get(parameters, "value", 1.0f));
    } else if (name == "sepia") {
        filter = make_sepia_filter(Rml::Get(parameters, "value", 1.0f));
    } else if (name == "hue-rotate") {
        filter = make_hue_rotate_filter(Rml::Get(parameters, "value", 0.0f));
    } else if (name == "saturate") {
        filter = make_saturate_filter(Rml::Get(parameters, "value", 1.0f));
    }
    if (filter.kind == FilterKind::Invalid) {
        std::fprintf(stderr, "[rmlui] unsupported filter '%s'\n", name.c_str());
        return 0;
    }
    // A zero handle is a compile failure to RmlUi. Keep supported no-op filters
    // as valid handles; the render-time filter simplifier removes them later.
    const Rml::CompiledFilterHandle handle = ++m_impl->filter_counter;
    m_impl->filters.emplace(handle, filter);
    return handle;
}

void RenderInterface::ReleaseFilter(Rml::CompiledFilterHandle filter)
{
    auto filter_it = m_impl->filters.find(filter);
    if (filter_it == m_impl->filters.end())
        return;
    if (filter_it->second.kind == FilterKind::MaskImage && filter_it->second.resource != 0) {
        const Rml::TextureHandle texture = Rml::TextureHandle(filter_it->second.resource);
        auto texture_it = m_impl->textures.find(texture);
        if (texture_it != m_impl->textures.end() &&
            mask_filter_owns_saved_texture(texture_it->second.ownership)) {
            if (texture_ownership_releases_handle(texture_it->second.ownership) &&
                bgfx::isValid(texture_it->second.handle)) {
                bgfx::destroy(texture_it->second.handle);
            }
            m_impl->textures.erase(texture_it);
        }
        filter_it->second.resource = 0;
    }
    m_impl->filters.erase(filter_it);
}

Rml::CompiledShaderHandle RenderInterface::CompileShader(const Rml::String& name,
                                                         const Rml::Dictionary& parameters)
{
    GradientRecord gradient = make_invalid_gradient();
    if (populate_gradient(gradient, name, parameters)) {
        const Rml::CompiledShaderHandle handle = ++m_impl->shader_counter;
        ShaderRecord record;
        record.kind = ShaderRecordKind::Gradient;
        record.gradient = gradient;
        m_impl->shaders.emplace(handle, std::move(record));
        return handle;
    }

    if (name == "shader") {
        const Rml::String value = Rml::Get(parameters, "value", Rml::String());
        const Rml::Vector2f dimensions = Rml::Get(parameters, "dimensions", Rml::Vector2f(0.0f));
        if (value.empty()) {
            m_impl->log_warning("shader() decorator is missing a material id value");
            return 0;
        }
        if (!m_impl->material_shader_provider) {
            m_impl->log_warning(
                "shader() decorator requested but no material shader provider is configured");
            return 0;
        }
        const RmlUiMaterialShaderHandle material =
            m_impl->material_shader_provider->compile_decorator_shader(
                RmlUiMaterialShaderRequest{value, dimensions});
        if (!material.valid())
            return 0;

        const Rml::CompiledShaderHandle handle = ++m_impl->shader_counter;
        ShaderRecord record;
        record.kind = ShaderRecordKind::Material;
        record.material = material;
        record.paint_dimensions = dimensions;
        record.value = value;
        m_impl->shaders.emplace(handle, std::move(record));
        return handle;
    }

    std::fprintf(stderr, "[rmlui] shader '%s' is not supported by the bgfx renderer\n",
                 name.c_str());
    return 0;
}

void RenderInterface::RenderShader(Rml::CompiledShaderHandle shader,
                                   Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                                   Rml::TextureHandle texture)
{
    auto shader_it = m_impl->shaders.find(shader);
    auto geometry_it = m_impl->geometries.find(geometry);
    if (shader_it == m_impl->shaders.end() || geometry_it == m_impl->geometries.end())
        return;
    if (m_impl->render_path == RenderPath::Reference) {
        m_impl->reference_renderer.render_shader(shader, geometry, translation, texture);
        return;
    }
    if (m_impl->active_layer_is_recording()) {
        m_impl->record_shader_command(shader, geometry, translation, texture);
        return;
    }
    m_impl->submit_shader(shader_it->second, geometry_it->second, translation, texture);
}

void RenderInterface::ReleaseShader(Rml::CompiledShaderHandle shader)
{
    auto shader_it = m_impl->shaders.find(shader);
    if (shader_it == m_impl->shaders.end())
        return;
    m_impl->release_shader_record(shader_it->second);
    m_impl->shaders.erase(shader_it);
}

void RenderInterface::set_perf_logging_enabled(bool enabled)
{
    m_impl->perf_logging_enabled = enabled;
}

void RenderInterface::set_base_direct_compatibility(bool enabled)
{
    m_impl->base_direct_compatibility_enabled = enabled;
}

} // namespace rmlui_bgfx
