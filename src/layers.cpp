#include "rmlui_bgfx_layers.hpp"
#include "rmlui_bgfx_layer_composite_helpers.hpp"
#include "rmlui_bgfx_layer_paths.hpp"

#include <algorithm>

namespace rmlui_bgfx {

namespace {

[[nodiscard]] bool is_full_frame_surface(FbRect rect, const SurfaceMetrics& surface)
{
    return !is_empty(rect) && rect.x == 0 && rect.y == 0 && rect.w >= surface.framebuffer_width &&
           rect.h >= surface.framebuffer_height;
}

} // namespace

BgfxLayerSystem::BgfxLayerSystem(BgfxTargetCache& target_cache) : m_target_cache(&target_cache) {}

void BgfxLayerSystem::begin_frame()
{
    if (m_target_cache) {
        m_target_cache->begin_frame();
    }
    m_layer_stack.clear();
    m_layer_stack.push_back(0);
    m_active_layer = 0;
}

void BgfxLayerSystem::clear_stack_to_base()
{
    m_layer_stack.clear();
    m_active_layer = 0;
}

void BgfxLayerSystem::push_layer(Rml::LayerHandle handle)
{
    m_layer_stack.push_back(handle);
    m_active_layer = handle;
}

bool BgfxLayerSystem::pop_layer()
{
    if (m_layer_stack.size() <= 1) {
        return false;
    }
    m_layer_stack.pop_back();
    m_active_layer = m_layer_stack.back();
    return true;
}

LayerRecord& BgfxLayerSystem::prepare_virtual_child(Rml::LayerHandle handle,
                                                    Rml::LayerHandle parent,
                                                    const RenderBounds& provisional_bounds,
                                                    ScissorState push_scissor,
                                                    bool push_transform_valid)
{
    LayerRecord& previous = m_target_cache->prepare_virtual_layer_slot(uint32_t(handle));
    LayerRecord preserved_resources;
    preserved_resources.framebuffer = previous.framebuffer;
    preserved_resources.color = previous.color;
    preserved_resources.depth_stencil = previous.depth_stencil;
    preserved_resources.texture_width = previous.texture_width;
    preserved_resources.texture_height = previous.texture_height;
    previous.framebuffer = BGFX_INVALID_HANDLE;
    previous.color = BGFX_INVALID_HANDLE;
    previous.depth_stencil = BGFX_INVALID_HANDLE;
    previous.texture_width = 0;
    previous.texture_height = 0;

    LayerRecord child;
    child.framebuffer = preserved_resources.framebuffer;
    child.color = preserved_resources.color;
    child.depth_stencil = preserved_resources.depth_stencil;
    child.texture_width = preserved_resources.texture_width;
    child.texture_height = preserved_resources.texture_height;
    child.kind = LayerKind::VirtualChild;
    child.parent_layer = parent;
    child.bounds = provisional_bounds;
    child.push_scissor = push_scissor;
    child.push_transform_valid = push_transform_valid;
    child.recording = true;
    child.materialized = false;
    child.clear_pending = true;

    if (const LayerRecord* parent_layer = layer_for_handle(parent)) {
        child.clip_mask_enabled = parent_layer->clip_mask_enabled;
        child.stencil_ref =
            parent_layer->clip_mask_enabled ? parent_layer->stencil_ref : uint8_t(1);
        if (parent_layer->clip_mask_enabled) {
            child.conservative_mask_bounds = parent_layer->conservative_mask_bounds;
            child.clip_commands = parent_layer->clip_commands;
            child.inherited_clip_command_count = child.clip_commands.size();
        }
    }

    previous = std::move(child);
    return previous;
}

LayerRecord* BgfxLayerSystem::layer_for_handle(Rml::LayerHandle handle)
{
    if (!m_target_cache) {
        return nullptr;
    }
    return m_target_cache->layer(uint32_t(handle));
}

const LayerRecord* BgfxLayerSystem::layer_for_handle(Rml::LayerHandle handle) const
{
    if (!m_target_cache) {
        return nullptr;
    }
    return m_target_cache->layer(uint32_t(handle));
}

LayerRecord* BgfxLayerSystem::materialized_layer_for_handle(Rml::LayerHandle handle,
                                                            bool direct_base_requested)
{
    LayerRecord* layer = layer_for_handle(handle);
    if (!layer) {
        return nullptr;
    }
    if (size_t(handle) == 0 && direct_base_requested) {
        return layer;
    }
    if (!bgfx::isValid(layer->framebuffer)) {
        return nullptr;
    }
    return layer;
}

const LayerRecord* BgfxLayerSystem::materialized_layer_for_handle(Rml::LayerHandle handle,
                                                                  bool direct_base_requested) const
{
    const LayerRecord* layer = layer_for_handle(handle);
    if (!layer) {
        return nullptr;
    }
    if (size_t(handle) == 0 && direct_base_requested) {
        return layer;
    }
    if (!bgfx::isValid(layer->framebuffer)) {
        return nullptr;
    }
    return layer;
}

LayerRecord* BgfxLayerSystem::current_layer() { return layer_for_handle(m_active_layer); }

const LayerRecord* BgfxLayerSystem::current_layer() const
{
    return layer_for_handle(m_active_layer);
}

bool BgfxLayerSystem::active_layer_is_recording() const
{
    const LayerRecord* layer = layer_for_handle(m_active_layer);
    if (!layer) {
        return false;
    }
    return layer->kind == LayerKind::VirtualChild && layer->recording && !layer->materialized;
}

bool BgfxLayerSystem::materialize_layer(const BgfxLayerMaterializeContext& ctx,
                                        Rml::LayerHandle handle,
                                        std::optional<FbRect> required_bounds)
{
    LayerRecord* layer = layer_for_handle(handle);
    if (!layer) {
        return false;
    }
    if (layer->kind == LayerKind::Root) {
        return true;
    }
    if (!ctx.choose_bounds || !ctx.ensure_layer || !ctx.clear_layer || !ctx.replay_clip_commands ||
        !ctx.replay_recorded_commands) {
        return false;
    }
    if (layer->materialized) {
        const FbRect current_overlap =
            required_bounds ? intersect(layer->bounds.framebuffer, *required_bounds) : FbRect{};
        if (!required_bounds || is_empty(*required_bounds) ||
            (current_overlap.x == required_bounds->x && current_overlap.y == required_bounds->y &&
             current_overlap.w == required_bounds->w && current_overlap.h == required_bounds->h)) {
            return true;
        }
        layer->materialized = false;
        layer->clear_pending = true;
    }

    RenderBounds child_bounds = ctx.choose_bounds(*layer, required_bounds);
    const bool bounded = !is_full_frame_surface(child_bounds.framebuffer, ctx.surface);
    if (!ctx.ensure_layer(size_t(handle), child_bounds)) {
        return false;
    }
    layer = layer_for_handle(handle);
    if (!layer) {
        return false;
    }
    layer->recording = false;
    layer->materialized = true;
    const bool final_clip_mask_enabled = layer->clip_mask_enabled;
    const uint8_t final_stencil_ref = layer->stencil_ref;

    if (layer->clear_pending) {
        if (!ctx.clear_layer(handle, bounded)) {
            return false;
        }
        layer = layer_for_handle(handle);
        if (!layer) {
            return false;
        }
        layer->clear_pending = false;
    }

    if (layer->inherited_clip_command_count > 0) {
        const size_t count =
            std::min(layer->inherited_clip_command_count, layer->clip_commands.size());
        const std::vector<size_t> inherited_commands(layer->clip_commands.begin(),
                                                     layer->clip_commands.begin() + count);
        ctx.replay_clip_commands(handle, inherited_commands);
        layer = layer_for_handle(handle);
        if (!layer) {
            return false;
        }
        layer->clip_mask_enabled = final_clip_mask_enabled;
        layer->stencil_ref = final_stencil_ref;
    }
    return ctx.replay_recorded_commands(handle);
}

void BgfxLayerSystem::composite_layers(const BgfxLayerCompositeContext& ctx,
                                       Rml::LayerHandle source, Rml::LayerHandle destination,
                                       Rml::BlendMode blend_mode,
                                       Rml::Span<const Rml::CompiledFilterHandle> filters)
{
    switch (ctx.render_path) {
    case RenderPath::Reference:
        return;
    case RenderPath::Optimized:
        composite_layers_optimized(*this, ctx, source, destination, blend_mode, filters);
        return;
    }
}

Rml::TextureHandle BgfxLayerSystem::save_layer_as_texture(const BgfxLayerSaveTextureContext& ctx)
{
    if (ctx.direct_base_requested && size_t(m_active_layer) == 0) {
        if (ctx.root_requires_preservation) {
            *ctx.root_requires_preservation = true;
        }
        if (ctx.fail_frame) {
            ctx.fail_frame("SaveLayerAsTexture requires offscreen root");
        }
        return 0;
    }
    if (!ctx.current_save_bounds) {
        return 0;
    }
    const Rml::Rectanglei bounds = ctx.current_save_bounds();
    if (bounds.Width() <= 0 || bounds.Height() <= 0) {
        return 0;
    }
    const FbRect global_bounds{bounds.Left(), bounds.Top(), bounds.Width(), bounds.Height()};

    if (!ctx.materialize_layer ||
        !ctx.materialize_layer(m_active_layer, std::optional<FbRect>{global_bounds})) {
        if (ctx.fail_frame) {
            ctx.fail_frame("SaveLayerAsTexture failed to materialize layer");
        }
        return 0;
    }
    LayerRecord* layer = materialized_layer_for_handle(m_active_layer, ctx.direct_base_requested);
    if (!layer || !bgfx::isValid(layer->color) || !ctx.copy_region_to_texture || !ctx.textures ||
        !ctx.texture_counter) {
        return 0;
    }
    const FbRect local_bounds = local_rect_for_layer(global_bounds, *layer);
    if (is_empty(local_bounds)) {
        return 0;
    }

    bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
    const Rml::Vector2i output_dimensions{bounds.Width(), bounds.Height()};
    if (local_bounds.w == global_bounds.w && local_bounds.h == global_bounds.h) {
        texture = ctx.copy_region_to_texture(layer->color, rectangle_from_fb(local_bounds),
                                             layer->texture_width, layer->texture_height,
                                             "RmlUi.SaveLayerAsTexture", true);
    } else if (ctx.copy_region_to_sized_texture) {
        const FbRect overlap_global = intersect(global_bounds, layer->bounds.framebuffer);
        const Rml::Vector2i destination_offset{overlap_global.x - global_bounds.x,
                                               overlap_global.y - global_bounds.y};
        texture = ctx.copy_region_to_sized_texture(layer->color, rectangle_from_fb(local_bounds),
                                                   layer->texture_width, layer->texture_height,
                                                   output_dimensions, destination_offset,
                                                   "RmlUi.SaveLayerAsTexture", true);
    }
    if (!bgfx::isValid(texture)) {
        if (ctx.fail_frame) {
            ctx.fail_frame("SaveLayerAsTexture failed to copy layer contents");
        }
        return 0;
    }

    const Rml::TextureHandle handle = ++(*ctx.texture_counter);
    ctx.textures->emplace(
        handle,
        TextureRecord{texture,
                      {bounds.Width(), bounds.Height()},
                      RenderBounds{{float(bounds.Left()), float(bounds.Top()),
                                    float(bounds.Width()), float(bounds.Height())},
                                   {bounds.Left(), bounds.Top(), bounds.Width(), bounds.Height()}},
                      TextureOwnership::SavedLayer});
    return handle;
}

Rml::CompiledFilterHandle
BgfxLayerSystem::save_layer_as_mask_image(const BgfxLayerSaveMaskContext& ctx)
{
    if (ctx.direct_base_requested && size_t(m_active_layer) == 0) {
        if (ctx.root_requires_preservation) {
            *ctx.root_requires_preservation = true;
        }
        if (ctx.fail_frame) {
            ctx.fail_frame("SaveLayerAsMaskImage requires offscreen root");
        }
        return 0;
    }
    if (!ctx.materialize_layer || !ctx.materialize_layer(m_active_layer, std::nullopt)) {
        if (ctx.fail_frame) {
            ctx.fail_frame("SaveLayerAsMaskImage failed to materialize layer");
        }
        return 0;
    }
    LayerRecord* layer = materialized_layer_for_handle(m_active_layer, ctx.direct_base_requested);
    if (!layer || !bgfx::isValid(layer->color) || !ctx.filters || !ctx.filter_counter ||
        !ctx.ensure_target || !ctx.composite) {
        return 0;
    }
    const FbRect mask_global_bounds =
        clamp_to_surface(align_outward_for_render_target(layer->bounds.framebuffer), ctx.surface);
    if (is_empty(mask_global_bounds)) {
        return 0;
    }

    RenderTargetRecord* blend_mask =
        ctx.ensure_target(PostprocessTargetKind::BlendMask, mask_global_bounds);
    if (!blend_mask || !bgfx::isValid(blend_mask->framebuffer) ||
        !bgfx::isValid(blend_mask->color)) {
        if (ctx.fail_frame) {
            ctx.fail_frame("SaveLayerAsMaskImage target allocation failed");
        }
        return 0;
    }

    TextureRegion source =
        make_layer_texture_region(layer->color, layer->bounds.framebuffer, full_local_rect(*layer),
                                  layer->texture_width, layer->texture_height);
    source = subregion(source, mask_global_bounds);
    if (is_empty(source.global_bounds) || is_empty(source.local_rect)) {
        return 0;
    }

    if (!ctx.composite(make_layer_composite_op(
            source, blend_mask->framebuffer, Rml::BlendMode::Replace, ScissorState{false, {}},
            false, 1, RmlUiPassKind::Copy, RmlUiPassReason::FilterMaskImage,
            "RmlUi.SaveLayerAsMaskImage",
            LocalFbRect{0, 0, blend_mask->texture_width, blend_mask->texture_height}))) {
        if (ctx.fail_frame) {
            ctx.fail_frame("SaveLayerAsMaskImage failed to copy layer contents");
        }
        return 0;
    }

    FilterRecord filter;
    filter.kind = FilterKind::MaskImage;
    filter.resource = 0;
    filter.mask_bounds = {mask_global_bounds.x, mask_global_bounds.y, mask_global_bounds.w,
                          mask_global_bounds.h};
    const Rml::CompiledFilterHandle handle = ++(*ctx.filter_counter);
    ctx.filters->emplace(handle, filter);
    return handle;
}

} // namespace rmlui_bgfx
