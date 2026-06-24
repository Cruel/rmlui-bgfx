#pragma once

#include "rmlui_bgfx_filters.hpp"
#include "rmlui_bgfx_target_cache.hpp"
#include "rmlui_bgfx_types.hpp"

#include <RmlUi/Core/Types.h>

#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

namespace rmlui_bgfx {

struct BgfxLayerMaterializeContext {
    SurfaceMetrics surface{};
    std::function<RenderBounds(const LayerRecord&, std::optional<FbRect>)> choose_bounds;
    std::function<bool(size_t, const RenderBounds&)> ensure_layer;
    std::function<bool(Rml::LayerHandle, bool)> clear_layer;
    std::function<void(Rml::LayerHandle, const std::vector<size_t>&)> replay_clip_commands;
    std::function<bool(Rml::LayerHandle)> replay_recorded_commands;
};

struct BgfxLayerCompositeContext {
    bool direct_base_requested = false;
    bool* root_requires_preservation = nullptr;
    SurfaceMetrics surface{};
    ScissorState scissor_state;
    BgfxFilterPipeline* filter_pipeline = nullptr;
    BgfxFilterPipelineContext filter_context;
    std::function<void(const char*)> fail_frame;
    std::function<FbRect(const LayerRecord&)> recorded_content_bounds;
    std::function<bool(Rml::LayerHandle, std::optional<FbRect>)> materialize_layer;
    std::function<RenderTargetRecord*(PostprocessTargetKind, const FbRect&)> ensure_target;
    std::function<bool(const CompositeOp&)> composite;
};

struct BgfxLayerSaveTextureContext {
    bool direct_base_requested = false;
    bool* root_requires_preservation = nullptr;
    std::unordered_map<Rml::TextureHandle, TextureRecord>* textures = nullptr;
    Rml::TextureHandle* texture_counter = nullptr;
    std::function<void(const char*)> fail_frame;
    std::function<bool(Rml::LayerHandle, std::optional<FbRect>)> materialize_layer;
    std::function<Rml::Rectanglei()> current_save_bounds;
    std::function<bgfx::TextureHandle(bgfx::TextureHandle, Rml::Rectanglei, int, int, const char*)>
        copy_region_to_texture;
};

struct BgfxLayerSaveMaskContext {
    SurfaceMetrics surface{};
    bool direct_base_requested = false;
    bool* root_requires_preservation = nullptr;
    std::unordered_map<Rml::TextureHandle, TextureRecord>* textures = nullptr;
    std::unordered_map<Rml::CompiledFilterHandle, FilterRecord>* filters = nullptr;
    Rml::TextureHandle* texture_counter = nullptr;
    Rml::CompiledFilterHandle* filter_counter = nullptr;
    std::function<void(const char*)> fail_frame;
    std::function<bool(Rml::LayerHandle, std::optional<FbRect>)> materialize_layer;
    std::function<bgfx::TextureHandle(bgfx::TextureHandle, Rml::Rectanglei, int, int, const char*)>
        copy_region_to_texture;
};

class BgfxLayerSystem {
public:
    explicit BgfxLayerSystem(BgfxTargetCache& target_cache);

    void begin_frame();
    void clear_stack_to_base();
    void push_layer(Rml::LayerHandle handle);
    bool pop_layer();
    LayerRecord& prepare_virtual_child(Rml::LayerHandle handle, Rml::LayerHandle parent,
                                       const RenderBounds& provisional_bounds,
                                       ScissorState push_scissor, bool push_transform_valid);
    bool materialize_layer(const BgfxLayerMaterializeContext& ctx, Rml::LayerHandle handle,
                           std::optional<FbRect> required_bounds = std::nullopt);
    void composite_layers(const BgfxLayerCompositeContext& ctx, Rml::LayerHandle source,
                          Rml::LayerHandle destination, Rml::BlendMode blend_mode,
                          Rml::Span<const Rml::CompiledFilterHandle> filters);
    [[nodiscard]] Rml::TextureHandle save_layer_as_texture(const BgfxLayerSaveTextureContext& ctx);
    [[nodiscard]] Rml::CompiledFilterHandle
    save_layer_as_mask_image(const BgfxLayerSaveMaskContext& ctx);

    [[nodiscard]] Rml::LayerHandle active_layer() const { return m_active_layer; }
    [[nodiscard]] Rml::LayerHandle& active_layer_ref() { return m_active_layer; }
    [[nodiscard]] const Rml::LayerHandle& active_layer_ref() const { return m_active_layer; }

    [[nodiscard]] std::vector<Rml::LayerHandle>& stack() { return m_layer_stack; }
    [[nodiscard]] const std::vector<Rml::LayerHandle>& stack() const { return m_layer_stack; }

    [[nodiscard]] LayerRecord* layer_for_handle(Rml::LayerHandle handle);
    [[nodiscard]] const LayerRecord* layer_for_handle(Rml::LayerHandle handle) const;
    [[nodiscard]] LayerRecord* materialized_layer_for_handle(Rml::LayerHandle handle,
                                                             bool direct_base_requested);
    [[nodiscard]] const LayerRecord*
    materialized_layer_for_handle(Rml::LayerHandle handle, bool direct_base_requested) const;
    [[nodiscard]] LayerRecord* current_layer();
    [[nodiscard]] const LayerRecord* current_layer() const;
    [[nodiscard]] bool active_layer_is_recording() const;

private:
    BgfxTargetCache* m_target_cache = nullptr;
    std::vector<Rml::LayerHandle> m_layer_stack;
    Rml::LayerHandle m_active_layer = 0;
};

} // namespace rmlui_bgfx
