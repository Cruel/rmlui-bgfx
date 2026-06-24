#include "rmlui_bgfx_layers.hpp"

#include <algorithm>

namespace rmlui_bgfx {

namespace {

[[nodiscard]] TextureRegion make_texture_region(bgfx::TextureHandle texture,
                                                GlobalFbRect global_bounds, LocalFbRect local_rect,
                                                int texture_width, int texture_height)
{
    return TextureRegion{texture, global_bounds, local_rect, texture_width, texture_height};
}

[[nodiscard]] CompositeOp
make_layer_composite_op(TextureRegion source, bgfx::FrameBufferHandle destination,
                        Rml::BlendMode blend_mode, ScissorState scissor,
                        bool apply_destination_stencil, uint8_t stencil_ref, RmlUiPassKind kind,
                        RmlUiPassReason reason, const char* name, LocalFbRect destination_rect = {},
                        CompositeFilterState filter = {})
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

[[nodiscard]] bool is_full_frame_surface(FbRect rect, const SurfaceMetrics& surface)
{
    return !is_empty(rect) && rect.x == 0 && rect.y == 0 && rect.w >= surface.framebuffer_width &&
           rect.h >= surface.framebuffer_height;
}

[[nodiscard]] Rml::Rectanglei clamp_scissor_to_surface(const Rml::Rectanglei& rect,
                                                       const SurfaceMetrics& surface)
{
    const int left = std::clamp(rect.Left(), 0, surface.framebuffer_width);
    const int top = std::clamp(rect.Top(), 0, surface.framebuffer_height);
    const int right = std::clamp(rect.Right(), 0, surface.framebuffer_width);
    const int bottom = std::clamp(rect.Bottom(), 0, surface.framebuffer_height);
    if (right <= left || bottom <= top) {
        return Rml::Rectanglei::FromPositionSize({0, 0}, {0, 0});
    }
    return Rml::Rectanglei::FromPositionSize({left, top}, {right - left, bottom - top});
}

[[nodiscard]] std::optional<FbRect>
filter_window_bounds(const BgfxLayerCompositeContext& ctx)
{
    if (ctx.scissor_state.enabled) {
        const Rml::Rectanglei scissor =
            clamp_scissor_to_surface(ctx.scissor_state.region, ctx.surface);
        if (scissor.Width() <= 0 || scissor.Height() <= 0) {
            return std::nullopt;
        }
        return FbRect{scissor.Left(), scissor.Top(), scissor.Width(), scissor.Height()};
    }
    return FbRect{0, 0, ctx.surface.framebuffer_width, ctx.surface.framebuffer_height};
}

[[nodiscard]] TextureRegion subregion(TextureRegion region, FbRect global_bounds)
{
    const FbRect clipped = intersect(global_bounds, region.global_bounds);
    if (is_empty(clipped)) {
        return {};
    }
    region.local_rect = {region.local_rect.x + clipped.x - region.global_bounds.x,
                         region.local_rect.y + clipped.y - region.global_bounds.y, clipped.w,
                         clipped.h};
    region.global_bounds = clipped;
    return region;
}

[[nodiscard]] bool composite_layers_gl3_compatible_filtered(
    BgfxLayerSystem& layer_system, const BgfxLayerCompositeContext& ctx, Rml::LayerHandle source,
    Rml::LayerHandle destination, Rml::BlendMode blend_mode,
    Rml::Span<const Rml::CompiledFilterHandle> filters)
{
    if (filters.empty()) {
        return false;
    }
    const std::optional<FbRect> window = filter_window_bounds(ctx);
    if (!window || is_empty(*window)) {
        return true;
    }

    LayerRecord* source_layer = layer_system.layer_for_handle(source);
    LayerRecord* destination_layer = layer_system.layer_for_handle(destination);
    if (!source_layer || !destination_layer) {
        if (ctx.fail_frame) {
            ctx.fail_frame("GL3-compatible CompositeLayers received invalid layer handles");
        }
        return true;
    }

    // This is deliberately separate from the optimized path. It mirrors the important GL3
    // semantic: filtered layer compositing operates over the current filter/scissor window,
    // not only over the tight recorded source-content bounds.
    if (!ctx.materialize_layer(source, *window)) {
        if (ctx.fail_frame) {
            ctx.fail_frame("GL3-compatible CompositeLayers failed to materialize source layer");
        }
        return true;
    }
    source_layer = layer_system.materialized_layer_for_handle(source, ctx.direct_base_requested);
    if (!source_layer) {
        if (ctx.fail_frame) {
            ctx.fail_frame("GL3-compatible CompositeLayers received unmaterialized source layer");
        }
        return true;
    }

    const bool source_clip_active = source_layer->clip_mask_enabled && ctx.replay_clip_commands &&
                                    !source_layer->clip_commands.empty();
    const uint8_t source_clip_ref = source_layer->stencil_ref;
    const std::vector<size_t> source_clip_commands =
        source_clip_active ? source_layer->clip_commands : std::vector<size_t>{};

    const FbRect source_window = intersect(*window, source_layer->bounds.framebuffer);
    if (is_empty(source_window)) {
        return true;
    }
    const FilterApplyResult filtered = ctx.filter_pipeline->apply(
        ctx.filter_context,
        make_texture_region(source_layer->color, source_window,
                            local_rect_for_layer(source_window, *source_layer),
                            source_layer->texture_width, source_layer->texture_height),
        source_layer->bounds, filters);
    if (!bgfx::isValid(filtered.output.texture)) {
        return true;
    }

    const FbRect final_global = intersect(filtered.output_bounds.framebuffer, *window);
    if (is_empty(final_global)) {
        return true;
    }
    if (!ctx.materialize_layer(destination, final_global)) {
        if (ctx.fail_frame) {
            ctx.fail_frame("GL3-compatible CompositeLayers failed to materialize destination layer");
        }
        return true;
    }
    destination_layer = layer_system.materialized_layer_for_handle(destination, ctx.direct_base_requested);
    if (!destination_layer) {
        if (ctx.fail_frame) {
            ctx.fail_frame("GL3-compatible CompositeLayers received unmaterialized destination layer");
        }
        return true;
    }

    if (source_clip_active) {
        ctx.replay_clip_commands(destination, source_clip_commands);
    }

    TextureRegion final_source = subregion(filtered.output, final_global);
    if (!bgfx::isValid(final_source.texture) || is_empty(final_source.local_rect)) {
        return true;
    }
    const FbRect destination_bounds = local_rect_for_layer(final_global, *destination_layer);
    if (is_empty(destination_bounds)) {
        return true;
    }

    const bool destination_clip = destination_layer->clip_mask_enabled || source_clip_active;
    const uint8_t destination_stencil_ref =
        source_clip_active ? source_clip_ref : destination_layer->stencil_ref;
    const ScissorState destination_scissor =
        scissor_local_to_layer(ctx.scissor_state, destination_layer->bounds);
    if (!ctx.composite(make_layer_composite_op(
            final_source, destination_layer->framebuffer, blend_mode, destination_scissor,
            destination_clip, destination_stencil_ref, RmlUiPassKind::LayerComposite,
            RmlUiPassReason::LayerComposite, "RmlUi.GL3CompatibleLayerComposite",
            destination_bounds, filtered.composite_filter))) {
        if (ctx.fail_frame) {
            ctx.fail_frame("GL3-compatible CompositeLayers composite failed");
        }
    }
    return true;
}

} // namespace

BgfxLayerSystem::BgfxLayerSystem(BgfxTargetCache& target_cache) : m_target_cache(&target_cache) {}

void BgfxLayerSystem::begin_frame()
{
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
        child.stencil_ref = parent_layer->stencil_ref;
        child.conservative_mask_bounds = parent_layer->conservative_mask_bounds;
        child.clip_commands = parent_layer->clip_commands;
        child.inherited_clip_command_count = child.clip_commands.size();
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
    if (layer->kind == LayerKind::Root || layer->materialized) {
        return true;
    }
    if (!ctx.choose_bounds || !ctx.ensure_layer || !ctx.clear_layer || !ctx.replay_clip_commands ||
        !ctx.replay_recorded_commands) {
        return false;
    }

    const RenderBounds child_bounds = ctx.choose_bounds(*layer, required_bounds);
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
    LayerRecord* source_layer = layer_for_handle(source);
    LayerRecord* destination_layer = layer_for_handle(destination);
    if (!source_layer || !destination_layer) {
        if (ctx.fail_frame) {
            ctx.fail_frame("CompositeLayers received invalid layer handles");
        }
        return;
    }
    if (ctx.direct_base_requested && size_t(destination) == 0 && !filters.empty()) {
        if (ctx.root_requires_preservation) {
            *ctx.root_requires_preservation = true;
        }
        if (ctx.fail_frame) {
            ctx.fail_frame("CompositeLayers root filters require offscreen presentation");
        }
        return;
    }
    if (!ctx.filter_pipeline || !ctx.recorded_content_bounds || !ctx.materialize_layer ||
        !ctx.ensure_target || !ctx.composite) {
        return;
    }
    if (ctx.filter_layer_composite_path == FilterLayerCompositePath::Gl3Compatible &&
        !filters.empty() &&
        composite_layers_gl3_compatible_filtered(*this, ctx, source, destination, blend_mode,
                                                 filters)) {
        return;
    }
    if (ctx.scissor_state.enabled) {
        const Rml::Rectanglei scissor =
            clamp_scissor_to_surface(ctx.scissor_state.region, ctx.surface);
        if (scissor.Width() <= 0 || scissor.Height() <= 0) {
            return;
        }
    }

    FbRect source_required = ctx.recorded_content_bounds(*source_layer);
    const FilterExpansion expansion =
        ctx.filter_pipeline->expansion_for(ctx.filter_context, filters);
    if (!is_empty(source_required)) {
        source_required = clamp_to_surface(
            align_outward_for_render_target(expand_bounds(source_required, expansion)),
            ctx.surface);
    }

    if (!ctx.materialize_layer(source, source_required)) {
        if (ctx.fail_frame) {
            ctx.fail_frame("CompositeLayers failed to materialize source layer");
        }
        return;
    }
    source_layer = materialized_layer_for_handle(source, ctx.direct_base_requested);
    if (!source_layer) {
        if (ctx.fail_frame) {
            ctx.fail_frame("CompositeLayers received unmaterialized source layer");
        }
        return;
    }

    const FbRect source_valid_global =
        source_layer->has_valid_content_bounds
            ? intersect(source_layer->valid_content_bounds, source_layer->bounds.framebuffer)
            : source_layer->bounds.framebuffer;

    if (source == destination) {
        const FbRect scratch_global_bounds = source_layer->bounds.framebuffer;
        RenderTargetRecord* scratch =
            ctx.ensure_target(PostprocessTargetKind::Scratch, scratch_global_bounds);
        if (!scratch) {
            if (ctx.fail_frame) {
                ctx.fail_frame("CompositeLayers failed to create scratch target");
            }
            return;
        }
        source_layer = materialized_layer_for_handle(source, ctx.direct_base_requested);
        destination_layer = materialized_layer_for_handle(destination, ctx.direct_base_requested);
        if (!source_layer || !destination_layer) {
            return;
        }
        const FbRect scratch_local_bounds{0, 0, scratch->texture_width, scratch->texture_height};
        if (!ctx.composite(make_layer_composite_op(
                make_texture_region(source_layer->color, source_layer->bounds.framebuffer,
                                    full_local_rect(*source_layer), source_layer->texture_width,
                                    source_layer->texture_height),
                scratch->framebuffer, Rml::BlendMode::Replace, ScissorState{false, {}}, false, 1,
                RmlUiPassKind::Copy, RmlUiPassReason::LayerScratchCopy, "RmlUi.LayerScratchCopy",
                scratch_local_bounds))) {
            if (ctx.fail_frame) {
                ctx.fail_frame("CompositeLayers scratch copy failed");
            }
            return;
        }
        const FilterApplyResult filtered = ctx.filter_pipeline->apply(
            ctx.filter_context,
            make_texture_region(
                scratch->color, source_valid_global,
                LocalFbRect{source_valid_global.x - source_layer->bounds.framebuffer.x,
                            source_valid_global.y - source_layer->bounds.framebuffer.y,
                            source_valid_global.w, source_valid_global.h},
                scratch->texture_width, scratch->texture_height),
            source_layer->bounds, filters);
        if (!bgfx::isValid(filtered.output.texture)) {
            return;
        }
        destination_layer = materialized_layer_for_handle(destination, ctx.direct_base_requested);
        if (!destination_layer) {
            return;
        }
        const bool destination_clip = destination_layer->clip_mask_enabled;
        const uint8_t destination_stencil_ref = destination_layer->stencil_ref;
        const ScissorState destination_scissor =
            scissor_local_to_layer(ctx.scissor_state, destination_layer->bounds);
        const FbRect destination_bounds =
            local_rect_for_layer(filtered.output_bounds.framebuffer, *destination_layer);
        if (is_empty(destination_bounds)) {
            return;
        }
        if (!ctx.composite(make_layer_composite_op(
                filtered.output, destination_layer->framebuffer, blend_mode, destination_scissor,
                destination_clip, destination_stencil_ref, RmlUiPassKind::LayerComposite,
                RmlUiPassReason::LayerComposite, "RmlUi.LayerComposite", destination_bounds,
                filtered.composite_filter))) {
            if (ctx.fail_frame) {
                ctx.fail_frame("CompositeLayers composite failed");
            }
            return;
        }
        return;
    }

    const FilterApplyResult filtered = ctx.filter_pipeline->apply(
        ctx.filter_context,
        make_texture_region(source_layer->color, source_valid_global,
                            local_rect_for_layer(source_valid_global, *source_layer),
                            source_layer->texture_width, source_layer->texture_height),
        source_layer->bounds, filters);
    if (!bgfx::isValid(filtered.output.texture)) {
        return;
    }

    if (!ctx.materialize_layer(destination, filtered.output_bounds.framebuffer)) {
        if (ctx.fail_frame) {
            ctx.fail_frame("CompositeLayers failed to materialize destination layer");
        }
        return;
    }
    destination_layer = materialized_layer_for_handle(destination, ctx.direct_base_requested);
    if (!destination_layer) {
        if (ctx.fail_frame) {
            ctx.fail_frame("CompositeLayers received unmaterialized destination layer");
        }
        return;
    }
    const bool destination_clip = destination_layer->clip_mask_enabled;
    const uint8_t destination_stencil_ref = destination_layer->stencil_ref;
    const ScissorState destination_scissor =
        scissor_local_to_layer(ctx.scissor_state, destination_layer->bounds);
    const FbRect destination_bounds =
        local_rect_for_layer(filtered.output_bounds.framebuffer, *destination_layer);
    if (is_empty(destination_bounds)) {
        return;
    }
    if (!ctx.composite(make_layer_composite_op(
            filtered.output, destination_layer->framebuffer, blend_mode, destination_scissor,
            destination_clip, destination_stencil_ref, RmlUiPassKind::LayerComposite,
            RmlUiPassReason::LayerComposite, "RmlUi.LayerComposite", destination_bounds,
            filtered.composite_filter))) {
        if (ctx.fail_frame) {
            ctx.fail_frame("CompositeLayers composite failed");
        }
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
    if (!layer || !bgfx::isValid(layer->color) || !ctx.copy_region_to_texture ||
        !ctx.textures || !ctx.texture_counter) {
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
        texture = ctx.copy_region_to_sized_texture(
            layer->color, rectangle_from_fb(local_bounds), layer->texture_width,
            layer->texture_height, output_dimensions, destination_offset,
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
    if (!layer || !bgfx::isValid(layer->color) || !ctx.copy_region_to_texture || !ctx.textures ||
        !ctx.filters || !ctx.texture_counter || !ctx.filter_counter) {
        return 0;
    }

    FbRect mask_global_bounds = layer->bounds.framebuffer;
    if (layer->has_valid_content_bounds) {
        const FbRect valid = intersect(layer->valid_content_bounds, layer->bounds.framebuffer);
        if (!is_empty(valid)) {
            mask_global_bounds = valid;
        }
    }
    const FbRect mask_local_bounds = local_rect_for_layer(mask_global_bounds, *layer);
    if (is_empty(mask_local_bounds)) {
        return 0;
    }

    bgfx::TextureHandle mask_texture = ctx.copy_region_to_texture(
        layer->color, rectangle_from_fb(mask_local_bounds), layer->texture_width,
        layer->texture_height, "RmlUi.SaveLayerAsMaskImage", false);
    if (!bgfx::isValid(mask_texture)) {
        if (ctx.fail_frame) {
            ctx.fail_frame("SaveLayerAsMaskImage failed to copy layer contents");
        }
        return 0;
    }

    const Rml::TextureHandle texture = ++(*ctx.texture_counter);
    ctx.textures->emplace(
        texture, TextureRecord{mask_texture,
                               {mask_local_bounds.w, mask_local_bounds.h},
                               RenderBounds{framebuffer_to_logical(mask_global_bounds, ctx.surface),
                                            mask_global_bounds},
                               TextureOwnership::SavedLayer});

    FilterRecord filter;
    filter.kind = FilterKind::MaskImage;
    filter.resource = texture;
    filter.mask_bounds = {mask_global_bounds.x, mask_global_bounds.y, mask_global_bounds.w,
                          mask_global_bounds.h};
    const Rml::CompiledFilterHandle handle = ++(*ctx.filter_counter);
    ctx.filters->emplace(handle, filter);
    return handle;
}

} // namespace rmlui_bgfx
