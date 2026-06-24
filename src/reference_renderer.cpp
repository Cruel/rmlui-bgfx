#include "rmlui_bgfx_reference_renderer.hpp"

#include "rmlui_bgfx_bounds.hpp"

#include <bx/math.h>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <span>

namespace rmlui_bgfx {

namespace {

constexpr uint64_t kReferenceColorTargetFlags =
    BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
constexpr uint64_t kReferenceDepthTargetFlags = BGFX_TEXTURE_RT_WRITE_ONLY;

[[nodiscard]] bool is_full_frame_rect(FbRect rect, const SurfaceMetrics& surface)
{
    return !is_empty(rect) && rect.x == 0 && rect.y == 0 &&
           rect.w >= surface.framebuffer_width && rect.h >= surface.framebuffer_height;
}

[[nodiscard]] Rml::Rectanglei rectangle_from_fb_rect(FbRect rect)
{
    return Rml::Rectanglei::FromPositionSize({rect.x, rect.y}, {rect.w, rect.h});
}

[[nodiscard]] FbRect full_frame_rect(const SurfaceMetrics& surface)
{
    return {0, 0, surface.framebuffer_width, surface.framebuffer_height};
}

[[nodiscard]] Rml::Rectanglei clamp_framebuffer_rect(Rml::Rectanglei region,
                                                     const SurfaceMetrics& surface)
{
    const int left = std::clamp(region.Left(), 0, std::max(surface.framebuffer_width, 0));
    const int top = std::clamp(region.Top(), 0, std::max(surface.framebuffer_height, 0));
    const int right = std::clamp(region.Right(), 0, std::max(surface.framebuffer_width, 0));
    const int bottom = std::clamp(region.Bottom(), 0, std::max(surface.framebuffer_height, 0));
    if (right <= left || bottom <= top) {
        return Rml::Rectanglei::FromPositionSize({0, 0}, {0, 0});
    }
    return Rml::Rectanglei::FromPositionSize({left, top}, {right - left, bottom - top});
}

[[nodiscard]] RenderBounds full_frame_bounds(const SurfaceMetrics& surface)
{
    const FbRect framebuffer = full_frame_rect(surface);
    return RenderBounds{{0.0f, 0.0f, float(surface.logical_width), float(surface.logical_height)},
                        framebuffer};
}

[[nodiscard]] ClipOperationPlan clip_operation_plan(Rml::ClipMaskOperation operation)
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

[[nodiscard]] const char* filter_kind_name(FilterKind kind)
{
    switch (kind) {
    case FilterKind::Opacity:
        return "opacity";
    case FilterKind::Blur:
        return "blur";
    case FilterKind::DropShadow:
        return "drop-shadow";
    case FilterKind::ColorMatrix:
        return "color-matrix";
    case FilterKind::MaskImage:
        return "mask-image";
    case FilterKind::Invalid:
        return "invalid";
    }
    return "unknown";
}

[[nodiscard]] RmlUiPassReason copy_pass_reason_from_name(const char* name)
{
    if (name && std::strcmp(name, "RmlUi.ReferenceSaveLayerAsTexture") == 0) {
        return RmlUiPassReason::SaveTextureCopy;
    }
    if (name && std::strcmp(name, "RmlUi.ReferenceSaveLayerAsMaskImage") == 0) {
        return RmlUiPassReason::SaveMaskCopy;
    }
    return RmlUiPassReason::OtherCopy;
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
    constexpr float samples_per_sigma = 12.0f;
    params.texel_scale = std::max(1.0f, desired_sigma / samples_per_sigma);
    params.sigma = desired_sigma;
    return params;
}

} // namespace

BgfxReferenceRenderer::~BgfxReferenceRenderer() { shutdown(); }

void BgfxReferenceRenderer::set_context(ReferenceRendererContext context)
{
    m_ctx = context;
}

void BgfxReferenceRenderer::resize()
{
    destroy_layers();
    destroy_targets();
}

void BgfxReferenceRenderer::shutdown()
{
    destroy_layers();
    destroy_targets();
    if (bgfx::isValid(m_fullscreen_vb)) {
        bgfx::destroy(m_fullscreen_vb);
        m_fullscreen_vb = BGFX_INVALID_HANDLE;
    }
}

void BgfxReferenceRenderer::begin_frame(const SurfaceMetrics& surface,
                                        bgfx::TextureFormat::Enum depth_stencil_format)
{
    m_surface = sanitize_surface_metrics(surface);
    m_depth_stencil_format = depth_stencil_format;
    m_frame_failed = false;
    m_scissor_enabled = false;
    m_scissor_region = Rml::Rectanglei::FromPositionSize(
        {0, 0}, {m_surface.framebuffer_width, m_surface.framebuffer_height});
    m_transform_valid = false;
    m_layer_pool.begin_frame();
    m_layer_stack.clear();
    m_clip_commands.clear();

    if (m_layers.empty()) {
        m_layers.resize(1);
        m_layers[0].handle = Rml::LayerHandle(0);
    }
    ReferenceLayer& root = m_layers[0];
    root.handle = Rml::LayerHandle(0);
    root.clip_mask_enabled = false;
    root.stencil_ref = 1;
    if (!ensure_layer_target(root)) {
        fail_frame("reference renderer failed to create root layer target");
        return;
    }
    m_layer_stack.push_back(Rml::LayerHandle(0));
    clear_layer(root, "RmlUi.ReferenceRootClear");
    if (m_ctx.perf) {
        m_ctx.perf->add_offscreen_base_presentation();
        m_ctx.perf->add_full_frame_layer();
    }
}

void BgfxReferenceRenderer::end_frame()
{
    if (m_frame_failed) {
        m_layer_stack.clear();
        m_layer_stack.push_back(Rml::LayerHandle(0));
        return;
    }
    if (m_layer_stack.size() != 1) {
        std::fprintf(stderr, "[rmlui] reference renderer unbalanced layer stack at frame end: %zu\n",
                     m_layer_stack.size());
        m_layer_stack.clear();
        m_layer_stack.push_back(Rml::LayerHandle(0));
    }
    ReferenceLayer* root = layer_for_handle(Rml::LayerHandle(0));
    if (!root || !bgfx::isValid(root->color)) {
        fail_frame("reference renderer missing root layer at frame end");
        return;
    }
    if (!submit_composite(layer_region(*root), BGFX_INVALID_HANDLE, Rml::BlendMode::Blend,
                          ScissorState{false, {}}, false, 1, RmlUiPassKind::FinalComposite,
                          RmlUiPassReason::FinalComposite, "RmlUi.ReferenceFinalComposite",
                          full_frame_rect(m_surface))) {
        fail_frame("reference renderer final composite failed");
    }
}

void BgfxReferenceRenderer::enable_scissor_region(bool enable)
{
    m_scissor_enabled = enable;
}

void BgfxReferenceRenderer::set_scissor_region(Rml::Rectanglei region)
{
    m_scissor_region = clamp_framebuffer_rect(region, m_surface);
}

void BgfxReferenceRenderer::set_transform(const float* transform)
{
    if (!transform) {
        m_transform_valid = false;
        return;
    }
    m_transform_valid = true;
    std::memcpy(m_transform, transform, sizeof(m_transform));
}

void BgfxReferenceRenderer::render_geometry(Rml::CompiledGeometryHandle geometry,
                                            Rml::Vector2f translation,
                                            Rml::TextureHandle texture)
{
    if (m_frame_failed || !m_ctx.geometries || !m_ctx.pass_builder || !m_ctx.draw_context) {
        return;
    }
    auto geometry_it = m_ctx.geometries->find(geometry);
    if (geometry_it == m_ctx.geometries->end()) {
        return;
    }
    ReferenceLayer* layer = active_layer();
    if (!layer || !bgfx::isValid(layer->framebuffer)) {
        return;
    }
    auto pass = m_ctx.pass_builder->geometry(layer->framebuffer, layer->width, layer->height,
                                             "RmlUi.ReferenceGeometry",
                                             RmlUiPassReason::OrdinaryGeometry);
    if (!pass) {
        return;
    }
    bgfx::TextureHandle bgfx_texture = m_ctx.white_texture;
    if (m_ctx.textures) {
        if (auto texture_it = m_ctx.textures->find(texture);
            texture_it != m_ctx.textures->end() && bgfx::isValid(texture_it->second.handle)) {
            bgfx_texture = texture_it->second.handle;
        }
    }
    const LayerRecord adapter = draw_layer_adapter(*layer);
    const bool submitted = m_ctx.draw_context->submit_geometry(
        *pass, draw_resources(), geometry_it->second, adapter,
        BgfxGeometryDrawState{translation,
                              bgfx_texture,
                              ScissorState{m_scissor_enabled, m_scissor_region},
                              m_transform_valid,
                              m_transform,
                              layer->clip_mask_enabled,
                              stencil_test_state_for_ref(layer->stencil_ref)});
    if (submitted && m_ctx.perf) {
        m_ctx.perf->add_geometry(uint64_t(layer->width) * uint64_t(layer->height),
                                 geometry_it->second.index_count);
    }
}

void BgfxReferenceRenderer::render_shader(Rml::CompiledShaderHandle shader,
                                          Rml::CompiledGeometryHandle geometry,
                                          Rml::Vector2f translation,
                                          Rml::TextureHandle texture)
{
    if (m_frame_failed || !m_ctx.geometries || !m_ctx.shaders || !m_ctx.pass_builder ||
        !m_ctx.draw_context) {
        return;
    }
    auto shader_it = m_ctx.shaders->find(shader);
    auto geometry_it = m_ctx.geometries->find(geometry);
    if (shader_it == m_ctx.shaders->end() || geometry_it == m_ctx.geometries->end()) {
        return;
    }
    ReferenceLayer* layer = active_layer();
    if (!layer || !bgfx::isValid(layer->framebuffer)) {
        return;
    }
    const ShaderRecord& shader_record = shader_it->second;
    const GeometryRecord& geometry_record = geometry_it->second;
    const LayerRecord adapter = draw_layer_adapter(*layer);

    switch (shader_record.kind) {
    case ShaderRecordKind::Gradient: {
        auto pass = m_ctx.pass_builder->geometry(layer->framebuffer, layer->width, layer->height,
                                                 "RmlUi.ReferenceGradient",
                                                 RmlUiPassReason::Gradient);
        if (!pass) {
            return;
        }
        const bool submitted = m_ctx.draw_context->submit_gradient(
            *pass, draw_resources(), shader_record, geometry_record, adapter,
            BgfxGradientDrawState{translation,
                                  ScissorState{m_scissor_enabled, m_scissor_region},
                                  m_transform_valid,
                                  m_transform,
                                  layer->clip_mask_enabled,
                                  stencil_test_state_for_ref(layer->stencil_ref)});
        if (submitted && m_ctx.perf) {
            m_ctx.perf->add_gradient();
        }
        break;
    }
    case ShaderRecordKind::Material: {
        if (!m_ctx.material_shaders || !shader_record.material.valid()) {
            return;
        }
        auto pass = m_ctx.pass_builder->geometry(layer->framebuffer, layer->width, layer->height,
                                                 "RmlUi.ReferenceMaterialShader",
                                                 RmlUiPassReason::OrdinaryGeometry);
        if (!pass) {
            return;
        }
        bgfx::TextureHandle bgfx_texture = m_ctx.white_texture;
        int texture_width = 1;
        int texture_height = 1;
        if (m_ctx.textures) {
            if (auto texture_it = m_ctx.textures->find(texture);
                texture_it != m_ctx.textures->end() && bgfx::isValid(texture_it->second.handle)) {
                bgfx_texture = texture_it->second.handle;
                texture_width = texture_it->second.dimensions.x;
                texture_height = texture_it->second.dimensions.y;
            }
        }
        Rml::Rectanglei local_scissor = Rml::Rectanglei::FromPositionSize({0, 0}, {0, 0});
        if (m_scissor_enabled) {
            local_scissor = clamp_scissor_local(m_scissor_region, layer->bounds.framebuffer);
            if (local_scissor.Width() <= 0 || local_scissor.Height() <= 0) {
                return;
            }
        }
        RmlUiMaterialShaderDrawContext context;
        context.view = pass->view;
        context.vertex_buffer = geometry_record.vb;
        context.index_buffer = geometry_record.ib;
        context.index_count = geometry_record.index_count;
        context.projection = layer->projection;
        context.transform = m_transform_valid ? m_transform : m_ctx.identity;
        context.translation = translation;
        context.scissor_enabled = m_scissor_enabled;
        context.local_scissor = local_scissor;
        context.clip_mask_enabled = layer->clip_mask_enabled;
        context.stencil_state = stencil_test_state_for_ref(layer->stencil_ref);
        context.texture = bgfx_texture;
        context.texture_width = texture_width;
        context.texture_height = texture_height;
        context.paint_dimensions = shader_record.paint_dimensions;
        context.dpi_scale = m_surface.scale_x;
        context.projection_uniform = m_ctx.uniforms.projection;
        context.transform_uniform = m_ctx.uniforms.transform;
        context.translate_uniform = m_ctx.uniforms.translate;
        context.white_texture = m_ctx.white_texture;
        context.premultiplied_blend_state = m_ctx.premultiplied_blend_state;
        if (m_ctx.material_shaders->submit_decorator_shader(shader_record.material, context) &&
            m_ctx.perf) {
            m_ctx.perf->add_geometry(uint64_t(layer->width) * uint64_t(layer->height),
                                     geometry_record.index_count);
        }
        break;
    }
    case ShaderRecordKind::Invalid:
        break;
    }
}

void BgfxReferenceRenderer::enable_clip_mask(bool enable)
{
    if (ReferenceLayer* layer = active_layer()) {
        layer->clip_mask_enabled = enable;
    }
}

void BgfxReferenceRenderer::render_to_clip_mask(Rml::ClipMaskOperation operation,
                                                Rml::CompiledGeometryHandle geometry,
                                                Rml::Vector2f translation)
{
    if (m_frame_failed || !m_ctx.geometries || m_ctx.geometries->find(geometry) == m_ctx.geometries->end()) {
        return;
    }
    ReferenceLayer* layer = active_layer();
    if (!layer) {
        return;
    }
    const StencilClipPlan clip_plan =
        plan_stencil_clip_operation(layer->stencil_ref, clip_operation_plan(operation));
    ReferenceClipCommand command;
    command.operation = operation;
    command.geometry = geometry;
    command.translation = translation;
    command.scissor = ScissorState{m_scissor_enabled, m_scissor_region};
    command.transform_valid = m_transform_valid;
    if (command.transform_valid) {
        std::memcpy(command.transform.data(), m_transform, sizeof(m_transform));
    }
    command.previous_ref = clip_plan.previous_ref;
    command.next_ref = clip_plan.next_ref;
    apply_clip_command(command, true);
}

Rml::LayerHandle BgfxReferenceRenderer::push_layer()
{
    if (m_frame_failed) {
        return Rml::LayerHandle(LayerPoolPlan::InvalidLayer);
    }
    ReferenceLayer* parent = active_layer();
    const Rml::LayerHandle parent_handle = parent ? parent->handle : Rml::LayerHandle(0);
    const bool parent_clip_mask_enabled = parent ? parent->clip_mask_enabled : false;
    const uint8_t parent_stencil_ref = parent ? parent->stencil_ref : 1;
    const std::vector<size_t> inherited_clip_commands =
        parent && parent_clip_mask_enabled ? parent->clip_commands : std::vector<size_t>{};

    const Rml::LayerHandle handle = Rml::LayerHandle(m_layer_pool.push());
    if (uint32_t(handle) == LayerPoolPlan::InvalidLayer) {
        return handle;
    }
    if (size_t(handle) >= m_layers.size()) {
        m_layers.resize(size_t(handle) + 1);
    }
    ReferenceLayer& layer = m_layers[size_t(handle)];
    layer.handle = handle;
    layer.clip_mask_enabled = parent_clip_mask_enabled;
    layer.stencil_ref = parent_stencil_ref;
    layer.clip_commands.clear();
    if (!ensure_layer_target(layer)) {
        fail_frame("reference renderer failed to create pushed layer target");
        return handle;
    }
    clear_layer(layer, "RmlUi.ReferenceLayerClear");
    m_layer_stack.push_back(handle);
    if (!inherited_clip_commands.empty()) {
        replay_clip_commands(handle, inherited_clip_commands);
    }
    if (m_ctx.perf) {
        m_ctx.perf->add_layer_push();
        m_ctx.perf->add_full_frame_child_layer();
    }
    trace("push layer=%zu parent=%zu inherited_clips=%zu target=full-frame %dx%d",
          size_t(handle), size_t(parent_handle), inherited_clip_commands.size(), layer.width,
          layer.height);
    return handle;
}

void BgfxReferenceRenderer::composite_layers(
    Rml::LayerHandle source, Rml::LayerHandle destination, Rml::BlendMode blend_mode,
    Rml::Span<const Rml::CompiledFilterHandle> filters)
{
    if (m_frame_failed || (source == destination && filters.empty())) {
        return;
    }
    ReferenceLayer* source_layer = layer_for_handle(source);
    ReferenceLayer* destination_layer = layer_for_handle(destination);
    if (!source_layer || !destination_layer || !bgfx::isValid(source_layer->color) ||
        !bgfx::isValid(destination_layer->framebuffer)) {
        fail_frame("reference renderer CompositeLayers received invalid layer handles");
        return;
    }

    FbRect work_rect = full_frame_rect(m_surface);
    if (m_scissor_enabled) {
        const Rml::Rectanglei save_bounds = current_save_bounds();
        work_rect = {save_bounds.Left(), save_bounds.Top(), save_bounds.Width(), save_bounds.Height()};
    }
    if (is_empty(work_rect)) {
        return;
    }

    ReferenceTextureRegion source_region = layer_region(*source_layer);
    source_region.global_bounds = work_rect;
    source_region.local_rect = {work_rect.x - source_layer->bounds.framebuffer.x,
                                work_rect.y - source_layer->bounds.framebuffer.y, work_rect.w,
                                work_rect.h};
    CompositeFilterState composite_filter{};
    if (source == destination && !filters.empty()) {
        ReferenceTarget* scratch = ensure_target(PostprocessTargetKind::Scratch, work_rect);
        if (!scratch) {
            fail_frame("reference renderer failed to allocate scratch target");
            return;
        }
        if (!submit_composite(source_region, scratch->framebuffer, Rml::BlendMode::Replace,
                              ScissorState{false, {}}, false, 1, RmlUiPassKind::Copy,
                              RmlUiPassReason::LayerScratchCopy,
                              "RmlUi.ReferenceLayerScratchCopy", {0, 0, scratch->width, scratch->height})) {
            fail_frame("reference renderer scratch copy failed");
            return;
        }
        source_region = target_region(*scratch);
    }

    if (!filters.empty()) {
        source_region = apply_filters(source_region, filters, &composite_filter);
        if (!bgfx::isValid(source_region.texture)) {
            fail_frame("reference renderer filter pipeline failed");
            return;
        }
    }

    bool apply_composite_stencil = destination_layer->clip_mask_enabled;
    uint8_t composite_stencil_ref = destination_layer->stencil_ref;
    if (!apply_composite_stencil && source_layer->clip_mask_enabled &&
        !source_layer->clip_commands.empty()) {
        replay_clip_commands(destination_layer->handle, source_layer->clip_commands);
        apply_composite_stencil = true;
        composite_stencil_ref = destination_layer->stencil_ref;
        trace("replay source clip for composite source=%zu destination=%zu clips=%zu ref=%u",
              size_t(source), size_t(destination), source_layer->clip_commands.size(),
              unsigned(composite_stencil_ref));
    }

    trace("composite source=%zu destination=%zu filters=%zu work=(%d,%d %dx%d) src_tex=%u src=%dx%d dst=%dx%d stencil=%d ref=%u",
          size_t(source), size_t(destination), size_t(filters.size()), work_rect.x, work_rect.y,
          work_rect.w, work_rect.h,
          bgfx::isValid(source_region.texture) ? source_region.texture.idx : 65535u,
          source_region.texture_width, source_region.texture_height, destination_layer->width,
          destination_layer->height, apply_composite_stencil ? 1 : 0,
          unsigned(composite_stencil_ref));
    if (!submit_composite(source_region, destination_layer->framebuffer, blend_mode,
                          ScissorState{m_scissor_enabled, m_scissor_region},
                          apply_composite_stencil, composite_stencil_ref,
                          RmlUiPassKind::LayerComposite, RmlUiPassReason::LayerComposite,
                          "RmlUi.ReferenceComposite", work_rect, composite_filter)) {
        fail_frame("reference renderer CompositeLayers composite failed");
    }
}

void BgfxReferenceRenderer::pop_layer()
{
    if (m_layer_stack.size() <= 1) {
        std::fprintf(stderr, "[rmlui] reference renderer attempted to pop the base layer\n");
        return;
    }
    trace("pop layer=%zu", size_t(m_layer_stack.back()));
    m_layer_stack.pop_back();
}

Rml::TextureHandle BgfxReferenceRenderer::save_layer_as_texture()
{
    if (m_frame_failed || !m_ctx.textures || !m_ctx.texture_counter) {
        return 0;
    }
    ReferenceLayer* layer = active_layer();
    if (!layer || !bgfx::isValid(layer->color)) {
        return 0;
    }
    const Rml::Rectanglei bounds = current_save_bounds();
    if (bounds.Width() <= 0 || bounds.Height() <= 0) {
        return 0;
    }
    bgfx::TextureHandle saved = copy_region_to_texture(
        layer->color, bounds, layer->width, layer->height, "RmlUi.ReferenceSaveLayerAsTexture",
        true);
    if (!bgfx::isValid(saved)) {
        fail_frame("reference renderer SaveLayerAsTexture copy failed");
        return 0;
    }
    const Rml::TextureHandle handle = ++(*m_ctx.texture_counter);
    const FbRect global_bounds{bounds.Left(), bounds.Top(), bounds.Width(), bounds.Height()};
    m_ctx.textures->emplace(
        handle,
        TextureRecord{saved,
                      {bounds.Width(), bounds.Height()},
                      RenderBounds{framebuffer_to_logical(global_bounds, m_surface), global_bounds},
                      TextureOwnership::SavedLayer});
    trace("save_texture layer=%zu bounds=(%d,%d %dx%d) rml_texture=%zu bgfx_tex=%u",
          size_t(layer->handle), global_bounds.x, global_bounds.y, global_bounds.w,
          global_bounds.h, size_t(handle), bgfx::isValid(saved) ? saved.idx : 65535u);
    return handle;
}

Rml::CompiledFilterHandle BgfxReferenceRenderer::save_layer_as_mask_image()
{
    if (m_frame_failed || !m_ctx.textures || !m_ctx.filters || !m_ctx.texture_counter ||
        !m_ctx.filter_counter) {
        return 0;
    }
    ReferenceLayer* layer = active_layer();
    if (!layer || !bgfx::isValid(layer->color)) {
        return 0;
    }
    const FbRect bounds = full_frame_rect(m_surface);
    bgfx::TextureHandle saved = copy_region_to_texture(
        layer->color, rectangle_from_fb_rect(bounds), layer->width, layer->height,
        "RmlUi.ReferenceSaveLayerAsMaskImage", false);
    if (!bgfx::isValid(saved)) {
        fail_frame("reference renderer SaveLayerAsMaskImage copy failed");
        return 0;
    }
    const Rml::TextureHandle texture = ++(*m_ctx.texture_counter);
    m_ctx.textures->emplace(texture,
                            TextureRecord{saved,
                                          {bounds.w, bounds.h},
                                          RenderBounds{framebuffer_to_logical(bounds, m_surface),
                                                       bounds},
                                          TextureOwnership::SavedLayer});
    FilterRecord filter;
    filter.kind = FilterKind::MaskImage;
    filter.resource = texture;
    filter.mask_bounds = {bounds.x, bounds.y, bounds.w, bounds.h};
    const Rml::CompiledFilterHandle handle = ++(*m_ctx.filter_counter);
    m_ctx.filters->emplace(handle, filter);
    trace("save_mask layer=%zu bounds=(%d,%d %dx%d) texture=%u filter=%zu",
          size_t(layer->handle), bounds.x, bounds.y, bounds.w, bounds.h,
          bgfx::isValid(saved) ? saved.idx : 65535u, size_t(handle));
    return handle;
}

bool BgfxReferenceRenderer::geometry_in_use(Rml::CompiledGeometryHandle geometry) const
{
    for (const ReferenceClipCommand& command : m_clip_commands) {
        if (command.geometry == geometry) {
            return true;
        }
    }
    return false;
}

ReferenceLayer* BgfxReferenceRenderer::active_layer()
{
    if (m_layer_stack.empty()) {
        return nullptr;
    }
    return layer_for_handle(m_layer_stack.back());
}

const ReferenceLayer* BgfxReferenceRenderer::active_layer() const
{
    if (m_layer_stack.empty()) {
        return nullptr;
    }
    return layer_for_handle(m_layer_stack.back());
}

ReferenceLayer* BgfxReferenceRenderer::layer_for_handle(Rml::LayerHandle handle)
{
    if (uint32_t(handle) == LayerPoolPlan::InvalidLayer || size_t(handle) >= m_layers.size()) {
        return nullptr;
    }
    ReferenceLayer& layer = m_layers[size_t(handle)];
    if (layer.handle != handle) {
        return nullptr;
    }
    return &layer;
}

const ReferenceLayer* BgfxReferenceRenderer::layer_for_handle(Rml::LayerHandle handle) const
{
    if (uint32_t(handle) == LayerPoolPlan::InvalidLayer || size_t(handle) >= m_layers.size()) {
        return nullptr;
    }
    const ReferenceLayer& layer = m_layers[size_t(handle)];
    if (layer.handle != handle) {
        return nullptr;
    }
    return &layer;
}

bool BgfxReferenceRenderer::ensure_fullscreen_geometry()
{
    if (bgfx::isValid(m_fullscreen_vb)) {
        return true;
    }
    if (!m_ctx.fullscreen_layout) {
        return false;
    }
    const bool origin_bottom_left = bgfx::getCaps() && bgfx::getCaps()->originBottomLeft;
    const auto vertices = fullscreen_triangle(origin_bottom_left);
    m_fullscreen_vb = bgfx::createVertexBuffer(
        bgfx::copy(vertices.data(), uint32_t(vertices.size() * sizeof(FullscreenVertex))),
        *m_ctx.fullscreen_layout);
    return bgfx::isValid(m_fullscreen_vb);
}

BgfxDrawResources BgfxReferenceRenderer::draw_resources() const
{
    return BgfxDrawResources{m_fullscreen_vb,
                             m_ctx.white_texture,
                             m_ctx.uniforms.sampler,
                             m_ctx.uniforms.mask_sampler,
                             m_ctx.uniforms.projection,
                             m_ctx.uniforms.transform,
                             m_ctx.uniforms.translate,
                             m_ctx.uniforms.gradient_params,
                             m_ctx.uniforms.gradient_stops,
                             m_ctx.uniforms.gradient_stop_meta,
                             m_ctx.uniforms.texcoord_bounds,
                             m_ctx.uniforms.mask_texcoord_transform,
                             m_ctx.uniforms.opacity,
                             m_ctx.uniforms.color_matrix,
                             m_ctx.uniforms.blur_params,
                             m_ctx.uniforms.blur_weights,
                             m_ctx.uniforms.shadow_color,
                             m_ctx.uniforms.shadow_offset,
                             m_ctx.programs.rmlui,
                             m_ctx.programs.composite,
                             m_ctx.programs.composite_filter,
                             m_ctx.programs.copy,
                             m_ctx.programs.gradient,
                             m_ctx.programs.mask_multiply,
                             m_ctx.programs.opacity,
                             m_ctx.programs.color_matrix,
                             m_ctx.programs.blur,
                             m_ctx.programs.drop_shadow,
                             m_ctx.identity,
                             m_ctx.premultiplied_blend_state};
}

LayerRecord BgfxReferenceRenderer::draw_layer_adapter(const ReferenceLayer& layer) const
{
    LayerRecord adapter;
    adapter.framebuffer = layer.framebuffer;
    adapter.color = layer.color;
    adapter.depth_stencil = layer.depth_stencil;
    adapter.bounds = layer.bounds;
    adapter.texture_width = layer.width;
    adapter.texture_height = layer.height;
    adapter.clip_mask_enabled = layer.clip_mask_enabled;
    adapter.stencil_ref = layer.stencil_ref;
    std::memcpy(adapter.projection, layer.projection, sizeof(adapter.projection));
    return adapter;
}

bool BgfxReferenceRenderer::ensure_layer_target(ReferenceLayer& layer)
{
    const RenderBounds bounds = full_frame_bounds(m_surface);
    if (bgfx::isValid(layer.framebuffer) && layer.width == bounds.framebuffer.w &&
        layer.height == bounds.framebuffer.h) {
        layer.bounds = bounds;
        bx::mtxOrtho(layer.projection, bounds.logical.x, bounds.logical.x + bounds.logical.w,
                     bounds.logical.y + bounds.logical.h, bounds.logical.y, -10000.0f, 10000.0f,
                     0.0f, bgfx::getCaps() ? bgfx::getCaps()->homogeneousDepth : false);
        return true;
    }
    destroy_layer(layer);
    if (m_depth_stencil_format == bgfx::TextureFormat::Unknown) {
        std::fprintf(stderr, "[rmlui] reference renderer requires a stencil-capable render target\n");
        return false;
    }
    bgfx::TextureHandle color = bgfx::createTexture2D(
        uint16_t(bounds.framebuffer.w), uint16_t(bounds.framebuffer.h), false, 1,
        bgfx::TextureFormat::RGBA8, kReferenceColorTargetFlags);
    bgfx::TextureHandle depth = bgfx::createTexture2D(
        uint16_t(bounds.framebuffer.w), uint16_t(bounds.framebuffer.h), false, 1,
        m_depth_stencil_format, kReferenceDepthTargetFlags);
    if (!bgfx::isValid(color) || !bgfx::isValid(depth)) {
        if (bgfx::isValid(color)) {
            bgfx::destroy(color);
        }
        if (bgfx::isValid(depth)) {
            bgfx::destroy(depth);
        }
        return false;
    }
    std::array<bgfx::TextureHandle, 2> attachments{color, depth};
    bgfx::FrameBufferHandle framebuffer =
        bgfx::createFrameBuffer(uint8_t(attachments.size()), attachments.data(), true);
    if (!bgfx::isValid(framebuffer)) {
        bgfx::destroy(color);
        bgfx::destroy(depth);
        return false;
    }
    layer.framebuffer = framebuffer;
    layer.color = color;
    layer.depth_stencil = depth;
    layer.bounds = bounds;
    layer.width = bounds.framebuffer.w;
    layer.height = bounds.framebuffer.h;
    bx::mtxOrtho(layer.projection, bounds.logical.x, bounds.logical.x + bounds.logical.w,
                 bounds.logical.y + bounds.logical.h, bounds.logical.y, -10000.0f, 10000.0f,
                 0.0f, bgfx::getCaps() ? bgfx::getCaps()->homogeneousDepth : false);
    m_layer_pool.note_allocated(uint32_t(layer.handle));
    if (m_ctx.perf) {
        m_ctx.perf->add_layer_alloc(uint32_t(layer.width), uint32_t(layer.height));
        m_ctx.perf->update_layer_max(uint32_t(layer.width), uint32_t(layer.height));
    }
    trace("allocate layer=%zu target=full-frame %dx%d", size_t(layer.handle), layer.width,
          layer.height);
    return true;
}

ReferenceTarget* BgfxReferenceRenderer::ensure_target(PostprocessTargetKind kind, FbRect bounds)
{
    const FbRect clamped = clamp_to_surface(align_outward_for_render_target(bounds), m_surface);
    if (is_empty(clamped)) {
        return nullptr;
    }
    for (ReferenceTarget& target : m_targets) {
        if (target.kind == kind && bgfx::isValid(target.framebuffer) && target.width == clamped.w &&
            target.height == clamped.h) {
            target.bounds = clamped;
            if (m_ctx.perf) {
                m_ctx.perf->add_postprocess_target_use(uint32_t(target.width),
                                                       uint32_t(target.height),
                                                       is_full_frame_rect(clamped, m_surface));
            }
            return &target;
        }
    }
    uint64_t flags = kReferenceColorTargetFlags;
    if (bgfx::getCaps() && (bgfx::getCaps()->supported & BGFX_CAPS_TEXTURE_BLIT) != 0) {
        flags |= BGFX_TEXTURE_BLIT_DST;
    }
    bgfx::TextureHandle color = bgfx::createTexture2D(uint16_t(clamped.w), uint16_t(clamped.h),
                                                      false, 1, bgfx::TextureFormat::RGBA8, flags);
    if (!bgfx::isValid(color)) {
        return nullptr;
    }
    bgfx::FrameBufferHandle framebuffer = bgfx::createFrameBuffer(1, &color, true);
    if (!bgfx::isValid(framebuffer)) {
        bgfx::destroy(color);
        return nullptr;
    }
    m_targets.push_back({framebuffer, color, clamped, clamped.w, clamped.h, kind});
    ReferenceTarget& target = m_targets.back();
    if (m_ctx.perf) {
        m_ctx.perf->add_pp_alloc(uint32_t(target.width), uint32_t(target.height));
        m_ctx.perf->add_postprocess_target_use(uint32_t(target.width), uint32_t(target.height),
                                               is_full_frame_rect(clamped, m_surface));
        if (is_full_frame_rect(clamped, m_surface)) {
            m_ctx.perf->add_full_frame_pp_target();
        } else {
            m_ctx.perf->add_bounded_pp_target();
        }
    }
    trace("allocate target kind=%u target=full-frame %dx%d", unsigned(kind), target.width,
          target.height);
    return &target;
}

void BgfxReferenceRenderer::destroy_layer(ReferenceLayer& layer)
{
    if (bgfx::isValid(layer.framebuffer)) {
        bgfx::destroy(layer.framebuffer);
        if (m_ctx.perf) {
            m_ctx.perf->add_layer_destroy();
        }
    }
    const Rml::LayerHandle handle = layer.handle;
    layer = {};
    layer.handle = handle;
}

void BgfxReferenceRenderer::destroy_target(ReferenceTarget& target)
{
    if (bgfx::isValid(target.framebuffer)) {
        bgfx::destroy(target.framebuffer);
        if (m_ctx.perf) {
            m_ctx.perf->add_pp_destroy();
        }
    }
    target = {};
}

void BgfxReferenceRenderer::destroy_layers()
{
    for (ReferenceLayer& layer : m_layers) {
        destroy_layer(layer);
    }
    m_layers.clear();
    m_layer_pool.reset_resources();
    m_layer_stack.clear();
    m_clip_commands.clear();
}

void BgfxReferenceRenderer::destroy_targets()
{
    for (ReferenceTarget& target : m_targets) {
        destroy_target(target);
    }
    m_targets.clear();
}

void BgfxReferenceRenderer::clear_layer(ReferenceLayer& layer, const char* name)
{
    if (!m_ctx.pass_builder || !bgfx::isValid(layer.framebuffer)) {
        return;
    }
    auto pass = m_ctx.pass_builder->layer_clear(layer.framebuffer, layer.width, layer.height);
    if (!pass) {
        return;
    }
    bgfx::setViewName(pass->view, name);
    bgfx::touch(pass->view);
    if (m_ctx.perf) {
        m_ctx.perf->add_layer_clear();
        m_ctx.perf->add_clear(uint64_t(layer.width) * uint64_t(layer.height), true);
    }
}

void BgfxReferenceRenderer::fail_frame(const char* message)
{
    if (!m_frame_failed && message) {
        std::fprintf(stderr, "[rmlui] %s\n", message);
    }
    m_frame_failed = true;
}

void BgfxReferenceRenderer::trace(const char* format, ...) const
{
    if (!m_ctx.trace) {
        return;
    }
    std::fprintf(stdout, "[rmlui-bgfx][reference] ");
    va_list args;
    va_start(args, format);
    std::vfprintf(stdout, format, args);
    va_end(args);
    std::fprintf(stdout, "\n");
    std::fflush(stdout);
}

uint32_t BgfxReferenceRenderer::stencil_test_state_for_ref(uint8_t ref) const
{
    return BGFX_STENCIL_TEST_EQUAL | BGFX_STENCIL_FUNC_REF(uint32_t(ref)) |
           BGFX_STENCIL_FUNC_RMASK(0xff) | BGFX_STENCIL_OP_FAIL_S_KEEP |
           BGFX_STENCIL_OP_FAIL_Z_KEEP | BGFX_STENCIL_OP_PASS_Z_KEEP;
}

uint32_t BgfxReferenceRenderer::stencil_replace_state(uint8_t value) const
{
    return BGFX_STENCIL_TEST_ALWAYS | BGFX_STENCIL_FUNC_REF(uint32_t(value)) |
           BGFX_STENCIL_FUNC_RMASK(0xff) | BGFX_STENCIL_OP_FAIL_S_KEEP |
           BGFX_STENCIL_OP_FAIL_Z_KEEP | BGFX_STENCIL_OP_PASS_Z_REPLACE;
}

uint32_t BgfxReferenceRenderer::stencil_intersect_state(uint8_t previous_ref) const
{
    return BGFX_STENCIL_TEST_EQUAL | BGFX_STENCIL_FUNC_REF(uint32_t(previous_ref)) |
           BGFX_STENCIL_FUNC_RMASK(0xff) | BGFX_STENCIL_OP_FAIL_S_KEEP |
           BGFX_STENCIL_OP_FAIL_Z_KEEP | BGFX_STENCIL_OP_PASS_Z_INCR;
}

uint32_t BgfxReferenceRenderer::stencil_decrement_state(uint8_t ref) const
{
    return BGFX_STENCIL_TEST_EQUAL | BGFX_STENCIL_FUNC_REF(uint32_t(ref)) |
           BGFX_STENCIL_FUNC_RMASK(0xff) | BGFX_STENCIL_OP_FAIL_S_KEEP |
           BGFX_STENCIL_OP_FAIL_Z_KEEP | BGFX_STENCIL_OP_PASS_Z_DECR;
}

FbRect BgfxReferenceRenderer::clip_work_bounds(const ReferenceLayer& layer,
                                               const ScissorState& scissor) const
{
    if (!scissor.enabled) {
        return {0, 0, layer.width, layer.height};
    }
    const Rml::Rectanglei local = clamp_scissor_local(scissor.region, layer.bounds.framebuffer);
    if (local.Width() <= 0 || local.Height() <= 0) {
        return {};
    }
    return {local.Left(), local.Top(), local.Width(), local.Height()};
}

void BgfxReferenceRenderer::clear_active_stencil(uint8_t value, const ScissorState& scissor)
{
    ReferenceLayer* layer = active_layer();
    if (!layer || !m_ctx.pass_builder) {
        return;
    }
    const FbRect work = clip_work_bounds(*layer, scissor);
    if (is_empty(work)) {
        return;
    }
    auto pass = m_ctx.pass_builder->stencil_clear(layer->framebuffer, work, value);
    if (!pass) {
        return;
    }
    bgfx::touch(pass->view);
    if (m_ctx.perf) {
        m_ctx.perf->add_clear(uint64_t(work.w) * uint64_t(work.h), is_full_frame_rect(work, m_surface));
    }
}

void BgfxReferenceRenderer::submit_clip_mask(
    const GeometryRecord& geometry, Rml::Vector2f translation, uint32_t stencil_state,
    const ScissorState& scissor, bool command_transform_valid,
    const std::array<float, 16>& command_transform)
{
    if (!m_ctx.pass_builder || !m_ctx.draw_context) {
        return;
    }
    ReferenceLayer* layer = active_layer();
    if (!layer || geometry.index_count == 0) {
        return;
    }
    const FbRect work = clip_work_bounds(*layer, scissor);
    if (is_empty(work)) {
        return;
    }
    auto pass = m_ctx.pass_builder->geometry(layer->framebuffer, layer->width, layer->height,
                                             "RmlUi.ReferenceClipMask",
                                             RmlUiPassReason::ClipMask);
    if (!pass) {
        return;
    }
    const LayerRecord adapter = draw_layer_adapter(*layer);
    const bool submitted = m_ctx.draw_context->submit_clip_mask(
        *pass, draw_resources(), geometry, adapter,
        BgfxClipMaskDrawState{translation, scissor, command_transform_valid,
                              command_transform.data(), stencil_state});
    if (submitted && m_ctx.perf) {
        m_ctx.perf->add_clip_mask(uint64_t(work.w) * uint64_t(work.h));
    }
}

void BgfxReferenceRenderer::apply_clip_command(const ReferenceClipCommand& command,
                                               bool record_on_layer)
{
    if (!m_ctx.geometries) {
        return;
    }
    auto geometry_it = m_ctx.geometries->find(command.geometry);
    if (geometry_it == m_ctx.geometries->end()) {
        return;
    }
    trace("clip operation=%d geometry=%zu previous_ref=%u next_ref=%u scissor=%d rect=(%d,%d %dx%d) transform=%d record=%d",
          int(command.operation), size_t(command.geometry), unsigned(command.previous_ref),
          unsigned(command.next_ref), command.scissor.enabled ? 1 : 0,
          command.scissor.region.Left(), command.scissor.region.Top(),
          command.scissor.region.Width(), command.scissor.region.Height(),
          command.transform_valid ? 1 : 0, record_on_layer ? 1 : 0);
    switch (command.operation) {
    case Rml::ClipMaskOperation::Set:
        clear_active_stencil(0, command.scissor);
        submit_clip_mask(geometry_it->second, command.translation, stencil_replace_state(1),
                         command.scissor, command.transform_valid, command.transform);
        break;
    case Rml::ClipMaskOperation::SetInverse:
        clear_active_stencil(1, command.scissor);
        submit_clip_mask(geometry_it->second, command.translation, stencil_replace_state(0),
                         command.scissor, command.transform_valid, command.transform);
        break;
    case Rml::ClipMaskOperation::Intersect:
        submit_clip_mask(geometry_it->second, command.translation,
                         stencil_intersect_state(command.previous_ref), command.scissor,
                         command.transform_valid, command.transform);
        break;
    }
    if (ReferenceLayer* layer = active_layer()) {
        layer->stencil_ref = command.next_ref;
        if (record_on_layer) {
            layer->clip_commands.push_back(m_clip_commands.size());
            m_clip_commands.push_back(command);
        }
    }
}

void BgfxReferenceRenderer::replay_clip_commands(Rml::LayerHandle layer,
                                                 const std::vector<size_t>& commands)
{
    const std::vector<Rml::LayerHandle> saved_stack = m_layer_stack;
    m_layer_stack.clear();
    m_layer_stack.push_back(layer);
    for (size_t index : commands) {
        if (index < m_clip_commands.size()) {
            apply_clip_command(m_clip_commands[index], false);
        }
    }
    m_layer_stack = saved_stack;
}

ReferenceTextureRegion BgfxReferenceRenderer::layer_region(const ReferenceLayer& layer) const
{
    return {layer.color, full_frame_rect(m_surface), {0, 0, layer.width, layer.height}, layer.width,
            layer.height};
}

ReferenceTextureRegion BgfxReferenceRenderer::target_region(const ReferenceTarget& target) const
{
    return {target.color, target.bounds, {0, 0, target.width, target.height}, target.width,
            target.height};
}

bool BgfxReferenceRenderer::texture_attached_to_framebuffer(
    bgfx::TextureHandle texture, bgfx::FrameBufferHandle framebuffer) const
{
    for (const ReferenceLayer& layer : m_layers) {
        if (bgfx::isValid(layer.framebuffer) && layer.framebuffer.idx == framebuffer.idx &&
            bgfx::isValid(layer.color) && layer.color.idx == texture.idx) {
            return true;
        }
    }
    for (const ReferenceTarget& target : m_targets) {
        if (bgfx::isValid(target.framebuffer) && target.framebuffer.idx == framebuffer.idx &&
            bgfx::isValid(target.color) && target.color.idx == texture.idx) {
            return true;
        }
    }
    return false;
}

bool BgfxReferenceRenderer::submit_composite(
    ReferenceTextureRegion source, bgfx::FrameBufferHandle destination, Rml::BlendMode blend_mode,
    ScissorState scissor, bool apply_destination_stencil, uint8_t stencil_ref, RmlUiPassKind kind,
    RmlUiPassReason reason, const char* name, FbRect destination_rect, CompositeFilterState filter)
{
    if (!m_ctx.pass_builder || !m_ctx.draw_context || !ensure_fullscreen_geometry() ||
        !bgfx::isValid(source.texture)) {
        return false;
    }
    if (bgfx::isValid(destination) && texture_attached_to_framebuffer(source.texture, destination)) {
        fail_frame("reference renderer composite feedback loop");
        return false;
    }
    const FbRect dst = is_empty(destination_rect) ? full_frame_rect(m_surface) : destination_rect;
    trace("submit_composite name=%s src_tex=%u src_global=(%d,%d %dx%d) src_local=(%d,%d %dx%d) src_size=%dx%d dst_fb=%u dst=(%d,%d %dx%d) blend=%d stencil=%d ref=%u filter=%d scissor=%d rect=(%d,%d %dx%d)",
          name ? name : "<null>", bgfx::isValid(source.texture) ? source.texture.idx : 65535u,
          source.global_bounds.x, source.global_bounds.y, source.global_bounds.w,
          source.global_bounds.h, source.local_rect.x, source.local_rect.y, source.local_rect.w,
          source.local_rect.h, source.texture_width, source.texture_height,
          bgfx::isValid(destination) ? destination.idx : 65535u, dst.x, dst.y, dst.w, dst.h,
          int(blend_mode), apply_destination_stencil ? 1 : 0, unsigned(stencil_ref),
          filter.enabled ? 1 : 0, scissor.enabled ? 1 : 0, scissor.region.Left(),
          scissor.region.Top(), scissor.region.Width(), scissor.region.Height());
    auto pass = m_ctx.pass_builder->composite(destination, dst, kind, name, reason);
    if (!pass) {
        return false;
    }
    CompositeOp op;
    op.source = TextureRegion{source.texture,
                              source.global_bounds,
                              source.local_rect,
                              source.texture_width,
                              source.texture_height};
    op.destination = destination;
    op.destination_rect = dst;
    op.blend_mode = blend_mode;
    op.scissor = scissor;
    op.apply_destination_stencil = apply_destination_stencil;
    op.stencil_ref = stencil_ref;
    op.kind = kind;
    op.reason = reason;
    op.name = name;
    op.filter = filter;
    const bool submitted = m_ctx.draw_context->submit_composite(
        *pass, draw_resources(), op, source.local_rect, dst, stencil_test_state_for_ref(stencil_ref));
    if (submitted && m_ctx.perf) {
        m_ctx.perf->add_composite(area(dst), is_full_frame_rect(dst, m_surface));
    }
    return submitted;
}

ReferenceTextureRegion BgfxReferenceRenderer::apply_filters(
    ReferenceTextureRegion source, Rml::Span<const Rml::CompiledFilterHandle> filter_handles,
    CompositeFilterState* composite_filter)
{
    if (composite_filter) {
        *composite_filter = {};
    }
    if (!m_ctx.filters || filter_handles.empty()) {
        return source;
    }

    std::vector<FilterRecord> filters;
    filters.reserve(filter_handles.size());
    for (Rml::CompiledFilterHandle handle : filter_handles) {
        auto it = m_ctx.filters->find(handle);
        if (it == m_ctx.filters->end()) {
            return {};
        }
        filters.push_back(it->second);
    }
    filters = simplify_filter_chain(std::span<const FilterRecord>(filters.data(), filters.size()));
    if (filters.empty()) {
        return source;
    }
    if (m_ctx.trace) {
        std::fprintf(stdout, "[rmlui-bgfx][reference] filters full-frame count=%zu", filters.size());
        for (const FilterRecord& filter : filters) {
            std::fprintf(stdout, " %s", filter_kind_name(filter.kind));
            if (filter.kind == FilterKind::Blur || filter.kind == FilterKind::DropShadow) {
                std::fprintf(stdout, " sigma=%.3f", filter.sigma);
            }
        }
        std::fprintf(stdout, "\n");
        std::fflush(stdout);
    }

    const ColorOnlyFilterPlan color_only_plan =
        plan_color_only_filter_chain(std::span<const FilterRecord>(filters.data(), filters.size()));
    if (color_only_plan.eligible) {
        if (composite_filter) {
            composite_filter->enabled = true;
            composite_filter->opacity = color_only_plan.opacity;
            composite_filter->color_matrix = color_only_plan.matrix;
        }
        if (m_ctx.perf) {
            m_ctx.perf->add_color_filter_composite_fold();
        }
        return source;
    }

    const FbRect work_rect = is_empty(source.global_bounds) ? full_frame_rect(m_surface)
                                                            : source.global_bounds;
    ReferenceTarget* primary = ensure_target(PostprocessTargetKind::Primary, work_rect);
    ReferenceTarget* secondary = ensure_target(PostprocessTargetKind::Secondary, work_rect);
    ReferenceTarget* tertiary = ensure_target(PostprocessTargetKind::Tertiary, work_rect);
    if (!primary || !secondary || !tertiary) {
        return {};
    }

    if (!submit_composite(source, primary->framebuffer, Rml::BlendMode::Replace,
                          ScissorState{false, {}}, false, 1, RmlUiPassKind::Copy,
                          RmlUiPassReason::FilterCopy, "RmlUi.ReferenceFilterCopy",
                          {0, 0, primary->width, primary->height})) {
        return {};
    }

    ReferenceTarget* current = primary;
    ReferenceTarget* destination = secondary;
    for (const FilterRecord& filter : filters) {
        bool ok = false;
        switch (filter.kind) {
        case FilterKind::Opacity: {
            const float opacity[4] = {filter.scalar, 0.0f, 0.0f, 0.0f};
            ok = fullscreen_postprocess(
                current->color, *destination, "RmlUi.ReferenceFilterOpacity",
                RmlUiPassReason::FilterOpacity, [&](const RmlUiPass& pass) {
                    return m_ctx.draw_context->submit_opacity(pass, draw_resources(), current->color,
                                                              opacity);
                });
            break;
        }
        case FilterKind::ColorMatrix:
            ok = fullscreen_postprocess(
                current->color, *destination, "RmlUi.ReferenceFilterColorMatrix",
                RmlUiPassReason::FilterColorMatrix, [&](const RmlUiPass& pass) {
                    return m_ctx.draw_context->submit_color_matrix(pass, draw_resources(),
                                                                   current->color,
                                                                   filter.matrix.data());
                });
            break;
        case FilterKind::MaskImage: {
            if (!m_ctx.textures) {
                return {};
            }
            auto tex_it = m_ctx.textures->find(Rml::TextureHandle(filter.resource));
            if (tex_it == m_ctx.textures->end() || !bgfx::isValid(tex_it->second.handle)) {
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
            ok = fullscreen_postprocess(
                current->color, *destination, "RmlUi.ReferenceFilterMaskImage",
                RmlUiPassReason::FilterMaskImage, [&](const RmlUiPass& pass) {
                    return m_ctx.draw_context->submit_mask_image(
                        pass, draw_resources(), current->color, tex_it->second.handle,
                        mask_transform, std::array<float, 4>{0.0f, 0.0f, 1.0f, 1.0f});
                });
            if (ok && m_ctx.perf) {
                m_ctx.perf->add_mask(uint64_t(destination->width) * uint64_t(destination->height),
                                     true);
            }
            break;
        }
        case FilterKind::Blur: {
            const BlurShaderParameters blur = blur_shader_parameters(filter.sigma);
            const float bounds[4] = {0.0f, 0.0f, 1.0f, 1.0f};
            float params[4] = {0.0f,
                               blur.texel_scale / float(std::max(destination->height, 1)),
                               blur.sigma,
                               blur.texel_scale};
            if (!fullscreen_postprocess(
                    current->color, *destination, "RmlUi.ReferenceFilterBlurV",
                    RmlUiPassReason::FilterBlur, [&](const RmlUiPass& pass) {
                        return m_ctx.draw_context->submit_blur(pass, draw_resources(),
                                                               current->color, params,
                                                               blur.weights, bounds);
                    })) {
                return {};
            }
            if (m_ctx.perf) {
                m_ctx.perf->add_blur();
            }
            std::swap(current, destination);
            params[0] = blur.texel_scale / float(std::max(destination->width, 1));
            params[1] = 0.0f;
            ok = fullscreen_postprocess(
                current->color, *destination, "RmlUi.ReferenceFilterBlurH",
                RmlUiPassReason::FilterBlur, [&](const RmlUiPass& pass) {
                    return m_ctx.draw_context->submit_blur(pass, draw_resources(), current->color,
                                                           params, blur.weights, bounds);
                });
            if (ok && m_ctx.perf) {
                m_ctx.perf->add_blur();
            }
            break;
        }
        case FilterKind::DropShadow: {
            const ReferenceTarget* original = current;
            const float color[4] = {filter.color[0], filter.color[1], filter.color[2],
                                    filter.color[3]};
            const float offset[4] = {
                filter.offset[0] / float(std::max(destination->width, 1)),
                filter.offset[1] / float(std::max(destination->height, 1)), 0.0f, 0.0f};
            if (!fullscreen_postprocess(
                    current->color, *destination, "RmlUi.ReferenceFilterDropShadowExtract",
                    RmlUiPassReason::FilterDropShadow, [&](const RmlUiPass& pass) {
                        return m_ctx.draw_context->submit_drop_shadow(
                            pass, draw_resources(), current->color, color, offset);
                    })) {
                return {};
            }
            if (m_ctx.perf) {
                m_ctx.perf->add_dropshadow();
            }
            std::swap(current, destination);
            if (filter.sigma >= 0.5f) {
                const BlurShaderParameters blur = blur_shader_parameters(filter.sigma);
                const float bounds[4] = {0.0f, 0.0f, 1.0f, 1.0f};
                float params[4] = {0.0f,
                                   blur.texel_scale / float(std::max(destination->height, 1)),
                                   blur.sigma,
                                   blur.texel_scale};
                if (!fullscreen_postprocess(
                        current->color, *destination, "RmlUi.ReferenceFilterDropShadowBlurV",
                        RmlUiPassReason::FilterBlur, [&](const RmlUiPass& pass) {
                            return m_ctx.draw_context->submit_blur(pass, draw_resources(),
                                                                   current->color, params,
                                                                   blur.weights, bounds);
                        })) {
                    return {};
                }
                if (m_ctx.perf) {
                    m_ctx.perf->add_blur();
                }
                std::swap(current, destination);
                params[0] = blur.texel_scale / float(std::max(destination->width, 1));
                params[1] = 0.0f;
                if (!fullscreen_postprocess(
                        current->color, *destination, "RmlUi.ReferenceFilterDropShadowBlurH",
                        RmlUiPassReason::FilterBlur, [&](const RmlUiPass& pass) {
                            return m_ctx.draw_context->submit_blur(pass, draw_resources(),
                                                                   current->color, params,
                                                                   blur.weights, bounds);
                        })) {
                    return {};
                }
                if (m_ctx.perf) {
                    m_ctx.perf->add_blur();
                }
                std::swap(current, destination);
            }
            ReferenceTextureRegion shadow_region = target_region(*current);
            if (!submit_composite(shadow_region, tertiary->framebuffer, Rml::BlendMode::Replace,
                                  ScissorState{false, {}}, false, 1, RmlUiPassKind::Postprocess,
                                  RmlUiPassReason::FilterDropShadowComposite,
                                  "RmlUi.ReferenceFilterDropShadowCopy",
                                  full_frame_rect(m_surface))) {
                return {};
            }
            if (!submit_composite(target_region(*original), tertiary->framebuffer,
                                  Rml::BlendMode::Blend, ScissorState{false, {}}, false, 1,
                                  RmlUiPassKind::Postprocess,
                                  RmlUiPassReason::FilterDropShadowComposite,
                                  "RmlUi.ReferenceFilterDropShadowComposite",
                                  full_frame_rect(m_surface))) {
                return {};
            }
            current = tertiary;
            destination = (original == primary) ? secondary : primary;
            ok = true;
            break;
        }
        case FilterKind::Invalid:
            return {};
        }
        if (!ok) {
            return {};
        }
        if (filter.kind != FilterKind::DropShadow) {
            std::swap(current, destination);
        }
    }
    return target_region(*current);
}

bool BgfxReferenceRenderer::fullscreen_postprocess(
    bgfx::TextureHandle source, const ReferenceTarget& destination, const char* name,
    RmlUiPassReason reason, const std::function<bool(const RmlUiPass&)>& submit)
{
    if (!m_ctx.pass_builder || !ensure_fullscreen_geometry() || !bgfx::isValid(source) ||
        !bgfx::isValid(destination.framebuffer)) {
        return false;
    }
    if (texture_attached_to_framebuffer(source, destination.framebuffer)) {
        fail_frame("reference renderer postprocess feedback loop");
        return false;
    }
    trace("postprocess name=%s source_tex=%u destination_fb=%u destination_tex=%u size=%dx%d reason=%d",
          name ? name : "<null>", bgfx::isValid(source) ? source.idx : 65535u,
          bgfx::isValid(destination.framebuffer) ? destination.framebuffer.idx : 65535u,
          bgfx::isValid(destination.color) ? destination.color.idx : 65535u, destination.width,
          destination.height, int(reason));
    auto pass = m_ctx.pass_builder->postprocess(destination.framebuffer, destination.width,
                                                destination.height, name, reason);
    if (!pass) {
        return false;
    }
    const bool ok = submit(*pass);
    if (ok && m_ctx.perf) {
        m_ctx.perf->add_postprocess(uint64_t(destination.width) * uint64_t(destination.height),
                                    true);
    }
    return ok;
}

bgfx::TextureHandle BgfxReferenceRenderer::copy_region_to_texture(
    bgfx::TextureHandle source, Rml::Rectanglei region, int source_width, int source_height,
    const char* name, bool flip_y)
{
    if (!bgfx::isValid(source) || region.Width() <= 0 || region.Height() <= 0 ||
        !m_ctx.pass_builder || !m_ctx.draw_context || !ensure_fullscreen_geometry()) {
        return BGFX_INVALID_HANDLE;
    }
    Rml::Rectanglei sample_region = region;
    const bool origin_bottom_left = bgfx::getCaps() && bgfx::getCaps()->originBottomLeft;
    if (flip_y && origin_bottom_left && source_height > region.Height()) {
        const int sample_top = source_height - region.Bottom();
        sample_region = Rml::Rectanglei::FromPositionSize(
            {region.Left(), sample_top}, {region.Width(), region.Height()});
    }
    const bool can_blit = !flip_y && bgfx::getCaps() &&
                          (bgfx::getCaps()->supported & BGFX_CAPS_TEXTURE_BLIT) != 0;
    trace("copy_region name=%s source_tex=%u source_size=%dx%d region=(%d,%d %dx%d) sample=(%d,%d %dx%d) flip_y=%d origin_bl=%d method=%s",
          name ? name : "<null>", bgfx::isValid(source) ? source.idx : 65535u, source_width,
          source_height, region.Left(), region.Top(), region.Width(), region.Height(),
          sample_region.Left(), sample_region.Top(), sample_region.Width(), sample_region.Height(),
          flip_y ? 1 : 0, origin_bottom_left ? 1 : 0, can_blit ? "blit" : "copy-pass");
    const uint64_t flags = (can_blit ? BGFX_TEXTURE_BLIT_DST : BGFX_TEXTURE_RT) |
                           BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
    bgfx::TextureHandle texture = bgfx::createTexture2D(
        uint16_t(region.Width()), uint16_t(region.Height()), false, 1, bgfx::TextureFormat::RGBA8,
        flags);
    if (!bgfx::isValid(texture)) {
        return BGFX_INVALID_HANDLE;
    }
    if (can_blit) {
        auto pass = m_ctx.pass_builder->copy(BGFX_INVALID_HANDLE, region.Width(), region.Height(),
                                             name, copy_pass_reason_from_name(name));
        if (!pass) {
            bgfx::destroy(texture);
            return BGFX_INVALID_HANDLE;
        }
        m_ctx.draw_context->submit_blit(*pass, texture, source, sample_region);
        if (m_ctx.perf) {
            m_ctx.perf->add_copy();
            m_ctx.perf->add_copy_pixels(uint64_t(region.Width()) * uint64_t(region.Height()));
        }
        return texture;
    }
    bgfx::FrameBufferHandle framebuffer = bgfx::createFrameBuffer(1, &texture, false);
    if (!bgfx::isValid(framebuffer)) {
        bgfx::destroy(texture);
        return BGFX_INVALID_HANDLE;
    }
    auto pass = m_ctx.pass_builder->copy(framebuffer, region.Width(), region.Height(), name,
                                         copy_pass_reason_from_name(name));
    if (!pass) {
        bgfx::destroy(framebuffer);
        bgfx::destroy(texture);
        return BGFX_INVALID_HANDLE;
    }
    const bool copied = m_ctx.draw_context->submit_copy(
        *pass, draw_resources(), source, sample_region, source_width, source_height, flip_y);
    bgfx::destroy(framebuffer);
    if (!copied) {
        bgfx::destroy(texture);
        return BGFX_INVALID_HANDLE;
    }
    if (m_ctx.perf) {
        m_ctx.perf->add_copy();
        m_ctx.perf->add_copy_pixels(uint64_t(region.Width()) * uint64_t(region.Height()));
    }
    return texture;
}

Rml::Rectanglei BgfxReferenceRenderer::current_save_bounds() const
{
    if (m_scissor_enabled) {
        return clamp_framebuffer_rect(m_scissor_region, m_surface);
    }
    return rectangle_from_fb_rect(full_frame_rect(m_surface));
}

} // namespace rmlui_bgfx
