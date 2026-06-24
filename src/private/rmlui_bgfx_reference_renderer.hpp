#pragma once

#include "rmlui_bgfx_draw.hpp"
#include "rmlui_bgfx_passes.hpp"
#include "rmlui_bgfx_planning.hpp"
#include "rmlui_bgfx_types.hpp"

#include <rmlui_bgfx/config.hpp>

#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/Types.h>
#include <bgfx/bgfx.h>

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rmlui_bgfx {

struct ReferenceRendererPrograms {
    bgfx::ProgramHandle rmlui = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle composite = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle composite_filter = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle copy = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle opacity = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle color_matrix = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle mask_multiply = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle blur = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle drop_shadow = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle gradient = BGFX_INVALID_HANDLE;
};

struct ReferenceRendererUniforms {
    bgfx::UniformHandle sampler = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle mask_sampler = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle projection = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle transform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle translate = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle color_matrix = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle opacity = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle gradient_params = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle gradient_stops = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle gradient_stop_meta = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle blur_params = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle blur_weights = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle texcoord_bounds = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle mask_texcoord_transform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle shadow_color = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle shadow_offset = BGFX_INVALID_HANDLE;
};

struct ReferenceRendererContext {
    std::unordered_map<Rml::CompiledGeometryHandle, GeometryRecord>* geometries = nullptr;
    std::unordered_map<Rml::TextureHandle, TextureRecord>* textures = nullptr;
    std::unordered_map<Rml::CompiledFilterHandle, FilterRecord>* filters = nullptr;
    std::unordered_map<Rml::CompiledShaderHandle, ShaderRecord>* shaders = nullptr;
    Rml::TextureHandle* texture_counter = nullptr;
    Rml::CompiledFilterHandle* filter_counter = nullptr;

    BgfxPassBuilder* pass_builder = nullptr;
    BgfxDrawContext* draw_context = nullptr;
    PerfCounters* perf = nullptr;
    MaterialShaderProvider* material_shaders = nullptr;

    const bgfx::VertexLayout* fullscreen_layout = nullptr;
    bgfx::TextureHandle white_texture = BGFX_INVALID_HANDLE;
    ReferenceRendererPrograms programs{};
    ReferenceRendererUniforms uniforms{};
    const float* identity = nullptr;
    uint64_t premultiplied_blend_state = 0;

    bool trace = false;
};

struct ReferenceLayer {
    Rml::LayerHandle handle = 0;
    bgfx::FrameBufferHandle framebuffer = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle color = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle depth_stencil = BGFX_INVALID_HANDLE;
    RenderBounds bounds{};
    int width = 0;
    int height = 0;
    float projection[16]{};
    bool clip_mask_enabled = false;
    uint8_t stencil_ref = 1;
    std::vector<size_t> clip_commands;
};

struct ReferenceTextureRegion {
    bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
    FbRect global_bounds{};
    FbRect local_rect{};
    int texture_width = 0;
    int texture_height = 0;
};

struct ReferenceTarget {
    bgfx::FrameBufferHandle framebuffer = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle color = BGFX_INVALID_HANDLE;
    FbRect bounds{};
    int width = 0;
    int height = 0;
    PostprocessTargetKind kind = PostprocessTargetKind::Primary;
};

struct ReferenceClipCommand {
    Rml::ClipMaskOperation operation = Rml::ClipMaskOperation::Set;
    Rml::CompiledGeometryHandle geometry = 0;
    Rml::Vector2f translation;
    ScissorState scissor;
    bool transform_valid = false;
    std::array<float, 16> transform{};
    uint8_t previous_ref = 1;
    uint8_t next_ref = 1;
};

class BgfxReferenceRenderer {
public:
    BgfxReferenceRenderer() = default;
    ~BgfxReferenceRenderer();

    BgfxReferenceRenderer(const BgfxReferenceRenderer&) = delete;
    BgfxReferenceRenderer& operator=(const BgfxReferenceRenderer&) = delete;

    void set_context(ReferenceRendererContext context);
    void resize();
    void shutdown();

    void begin_frame(const SurfaceMetrics& surface, bgfx::TextureFormat::Enum depth_stencil_format);
    void end_frame();

    void enable_scissor_region(bool enable);
    void set_scissor_region(Rml::Rectanglei region);
    void set_transform(const float* transform);

    void render_geometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                         Rml::TextureHandle texture);
    void render_shader(Rml::CompiledShaderHandle shader, Rml::CompiledGeometryHandle geometry,
                       Rml::Vector2f translation, Rml::TextureHandle texture);

    void enable_clip_mask(bool enable);
    void render_to_clip_mask(Rml::ClipMaskOperation operation, Rml::CompiledGeometryHandle geometry,
                             Rml::Vector2f translation);

    Rml::LayerHandle push_layer();
    void composite_layers(Rml::LayerHandle source, Rml::LayerHandle destination,
                          Rml::BlendMode blend_mode,
                          Rml::Span<const Rml::CompiledFilterHandle> filters);
    void pop_layer();

    Rml::TextureHandle save_layer_as_texture();
    Rml::CompiledFilterHandle save_layer_as_mask_image();

    [[nodiscard]] bool frame_failed() const { return m_frame_failed; }
    [[nodiscard]] size_t stack_size() const { return m_layer_stack.size(); }
    [[nodiscard]] bool geometry_in_use(Rml::CompiledGeometryHandle geometry) const;

private:
    [[nodiscard]] ReferenceLayer* active_layer();
    [[nodiscard]] const ReferenceLayer* active_layer() const;
    [[nodiscard]] ReferenceLayer* layer_for_handle(Rml::LayerHandle handle);
    [[nodiscard]] const ReferenceLayer* layer_for_handle(Rml::LayerHandle handle) const;

    [[nodiscard]] bool ensure_fullscreen_geometry();
    [[nodiscard]] BgfxDrawResources draw_resources() const;
    [[nodiscard]] LayerRecord draw_layer_adapter(const ReferenceLayer& layer) const;

    [[nodiscard]] bool ensure_layer_target(ReferenceLayer& layer);
    [[nodiscard]] ReferenceTarget* ensure_target(PostprocessTargetKind kind, FbRect bounds);
    void destroy_layer(ReferenceLayer& layer);
    void destroy_target(ReferenceTarget& target);
    void destroy_layers();
    void destroy_targets();

    void clear_layer(ReferenceLayer& layer, const char* name);
    void fail_frame(const char* message);
    void trace(const char* format, ...) const;

    [[nodiscard]] uint32_t stencil_test_state_for_ref(uint8_t ref) const;
    [[nodiscard]] uint32_t stencil_replace_state(uint8_t value) const;
    [[nodiscard]] uint32_t stencil_intersect_state(uint8_t previous_ref) const;
    [[nodiscard]] uint32_t stencil_decrement_state(uint8_t ref) const;
    [[nodiscard]] FbRect clip_work_bounds(const ReferenceLayer& layer, const ScissorState& scissor) const;
    void clear_active_stencil(uint8_t value, const ScissorState& scissor);
    void submit_clip_mask(const GeometryRecord& geometry, Rml::Vector2f translation,
                          uint32_t stencil_state, const ScissorState& scissor,
                          bool command_transform_valid, const std::array<float, 16>& command_transform);
    void apply_clip_command(const ReferenceClipCommand& command, bool record_on_layer);
    void replay_clip_commands(Rml::LayerHandle layer, const std::vector<size_t>& commands);

    [[nodiscard]] ReferenceTextureRegion layer_region(const ReferenceLayer& layer) const;
    [[nodiscard]] ReferenceTextureRegion target_region(const ReferenceTarget& target) const;
    [[nodiscard]] bool texture_attached_to_framebuffer(bgfx::TextureHandle texture,
                                                       bgfx::FrameBufferHandle framebuffer) const;
    [[nodiscard]] bool submit_composite(ReferenceTextureRegion source,
                                        bgfx::FrameBufferHandle destination,
                                        Rml::BlendMode blend_mode, ScissorState scissor,
                                        bool apply_destination_stencil, uint8_t stencil_ref,
                                        RmlUiPassKind kind, RmlUiPassReason reason,
                                        const char* name, FbRect destination_rect,
                                        CompositeFilterState filter = {});
    [[nodiscard]] ReferenceTextureRegion apply_filters(
        ReferenceTextureRegion source, Rml::Span<const Rml::CompiledFilterHandle> filters,
        CompositeFilterState* composite_filter);

    [[nodiscard]] bool fullscreen_postprocess(bgfx::TextureHandle source,
                                              const ReferenceTarget& destination,
                                              const char* name, RmlUiPassReason reason,
                                              const std::function<bool(const RmlUiPass&)>& submit);
    [[nodiscard]] bgfx::TextureHandle copy_region_to_texture(bgfx::TextureHandle source,
                                                             Rml::Rectanglei region,
                                                             int source_width,
                                                             int source_height,
                                                             const char* name, bool flip_y);
    [[nodiscard]] Rml::Rectanglei current_save_bounds() const;

    ReferenceRendererContext m_ctx{};
    SurfaceMetrics m_surface{};
    bgfx::TextureFormat::Enum m_depth_stencil_format = bgfx::TextureFormat::Unknown;
    bgfx::VertexBufferHandle m_fullscreen_vb = BGFX_INVALID_HANDLE;

    LayerPoolPlan m_layer_pool;
    std::vector<ReferenceLayer> m_layers;
    std::deque<ReferenceTarget> m_targets;
    std::vector<Rml::LayerHandle> m_layer_stack;
    std::vector<ReferenceClipCommand> m_clip_commands;

    bool m_frame_failed = false;
    bool m_scissor_enabled = false;
    Rml::Rectanglei m_scissor_region = Rml::Rectanglei::FromPositionSize({0, 0}, {0, 0});
    bool m_transform_valid = false;
    float m_transform[16]{};
};

} // namespace rmlui_bgfx
