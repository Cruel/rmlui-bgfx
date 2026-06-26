#include "rmlui_bgfx_layers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <unordered_map>
#include <vector>

using namespace rmlui_bgfx;

TEST_CASE("RmlUi saved mask image uses bounded blend mask target")
{
    BgfxTargetCache target_cache;
    BgfxLayerSystem layer_system(target_cache);
    layer_system.begin_frame();

    LayerRecord& layer = target_cache.prepare_virtual_layer_slot(1);
    layer.framebuffer = bgfx::FrameBufferHandle{3};
    layer.color = bgfx::TextureHandle{4};
    layer.bounds = RenderBounds{{0.0f, 0.0f, 200.0f, 200.0f}, {0, 0, 200, 200}};
    layer.valid_content_bounds = {30, 40, 20, 15};
    layer.has_valid_content_bounds = true;
    layer.texture_width = 200;
    layer.texture_height = 200;
    layer.kind = LayerKind::VirtualChild;
    layer.materialized = true;

    layer_system.push_layer(1);

    std::unordered_map<Rml::TextureHandle, TextureRecord> textures;
    std::unordered_map<Rml::CompiledFilterHandle, FilterRecord> filters;
    Rml::TextureHandle texture_counter = 7;
    Rml::CompiledFilterHandle filter_counter = 11;
    std::optional<FbRect> materialize_required_bounds;
    PostprocessTargetKind requested_target_kind = PostprocessTargetKind::Primary;
    FbRect requested_target_bounds{};
    RenderTargetRecord blend_mask;
    blend_mask.framebuffer = bgfx::FrameBufferHandle{5};
    blend_mask.color = bgfx::TextureHandle{6};
    blend_mask.bounds = {30, 40, 20, 15};
    blend_mask.texture_width = 20;
    blend_mask.texture_height = 15;
    blend_mask.kind = PostprocessTargetKind::BlendMask;
    bool composite_called = false;
    CompositeOp composite_op;

    BgfxLayerSaveMaskContext ctx;
    ctx.surface = SurfaceMetrics{200, 200, 200, 200, 1.0f, 1.0f};
    ctx.textures = &textures;
    ctx.filters = &filters;
    ctx.texture_counter = &texture_counter;
    ctx.filter_counter = &filter_counter;
    ctx.materialize_layer = [&](Rml::LayerHandle, std::optional<FbRect> required_bounds) {
        materialize_required_bounds = required_bounds;
        return true;
    };
    ctx.current_save_bounds = [] { return Rml::Rectanglei::FromPositionSize({30, 40}, {20, 15}); };
    ctx.copy_region_to_texture = [](bgfx::TextureHandle, Rml::Rectanglei, int, int, const char*,
                                    bool) { return bgfx::TextureHandle{9}; };
    ctx.ensure_target = [&](PostprocessTargetKind kind, const FbRect& bounds) {
        requested_target_kind = kind;
        requested_target_bounds = bounds;
        return &blend_mask;
    };
    ctx.composite = [&](const CompositeOp& op) {
        composite_called = true;
        composite_op = op;
        return true;
    };

    const Rml::CompiledFilterHandle filter = layer_system.save_layer_as_mask_image(ctx);

    CHECK(filter == 12);
    REQUIRE(filters.contains(filter));
    CHECK(textures.empty());
    REQUIRE(materialize_required_bounds.has_value());
    CHECK(materialize_required_bounds->x == 30);
    CHECK(materialize_required_bounds->y == 40);
    CHECK(materialize_required_bounds->w == 20);
    CHECK(materialize_required_bounds->h == 15);
    CHECK(requested_target_kind == PostprocessTargetKind::BlendMask);
    CHECK(requested_target_bounds.x == 30);
    CHECK(requested_target_bounds.y == 40);
    CHECK(requested_target_bounds.w == 20);
    CHECK(requested_target_bounds.h == 15);
    CHECK(composite_called);
    CHECK(composite_op.source.texture.idx == 4);
    CHECK(composite_op.destination.idx == 5);
    CHECK(composite_op.destination_rect.x == 0);
    CHECK(composite_op.destination_rect.y == 0);
    CHECK(composite_op.destination_rect.w == 20);
    CHECK(composite_op.destination_rect.h == 15);
    CHECK(composite_op.blend_mode == Rml::BlendMode::Replace);

    const FilterRecord& record = filters.at(filter);
    CHECK(record.kind == FilterKind::MaskImage);
    CHECK(record.resource == 0);
    CHECK(record.mask_bounds[0] == 30);
    CHECK(record.mask_bounds[1] == 40);
    CHECK(record.mask_bounds[2] == 20);
    CHECK(record.mask_bounds[3] == 15);

    layer.framebuffer = BGFX_INVALID_HANDLE;
}

TEST_CASE("RmlUi saved layer texture materializes requested save bounds")
{
    BgfxTargetCache target_cache;
    BgfxLayerSystem layer_system(target_cache);
    layer_system.begin_frame();

    LayerRecord& layer = target_cache.prepare_virtual_layer_slot(1);
    layer.framebuffer = bgfx::FrameBufferHandle{3};
    layer.color = bgfx::TextureHandle{4};
    layer.bounds = RenderBounds{{0.0f, 0.0f, 200.0f, 200.0f}, {0, 0, 200, 200}};
    layer.valid_content_bounds = {50, 60, 20, 20};
    layer.has_valid_content_bounds = true;
    layer.texture_width = 200;
    layer.texture_height = 200;
    layer.kind = LayerKind::VirtualChild;
    layer.materialized = true;

    layer_system.push_layer(1);

    std::unordered_map<Rml::TextureHandle, TextureRecord> textures;
    Rml::TextureHandle texture_counter = 10;
    std::optional<FbRect> materialize_required_bounds;
    Rml::Rectanglei copied_region = Rml::Rectanglei::FromPositionSize({0, 0}, {0, 0});

    BgfxLayerSaveTextureContext ctx;
    ctx.textures = &textures;
    ctx.texture_counter = &texture_counter;
    ctx.current_save_bounds = [] { return Rml::Rectanglei::FromPositionSize({10, 20}, {100, 80}); };
    ctx.materialize_layer = [&](Rml::LayerHandle, std::optional<FbRect> required_bounds) {
        materialize_required_bounds = required_bounds;
        return true;
    };
    ctx.copy_region_to_texture = [&](bgfx::TextureHandle, Rml::Rectanglei region, int, int,
                                     const char*, bool flip_y) {
        copied_region = region;
        CHECK(flip_y);
        return bgfx::TextureHandle{9};
    };

    const Rml::TextureHandle texture = layer_system.save_layer_as_texture(ctx);

    REQUIRE(texture == 11);
    REQUIRE(materialize_required_bounds.has_value());
    CHECK(materialize_required_bounds->x == 10);
    CHECK(materialize_required_bounds->y == 20);
    CHECK(materialize_required_bounds->w == 100);
    CHECK(materialize_required_bounds->h == 80);
    CHECK(copied_region.Left() == 10);
    CHECK(copied_region.Top() == 20);
    CHECK(copied_region.Width() == 100);
    CHECK(copied_region.Height() == 80);
    REQUIRE(textures.contains(texture));
    CHECK(textures.at(texture).dimensions.x == 100);
    CHECK(textures.at(texture).dimensions.y == 80);

    layer.framebuffer = BGFX_INVALID_HANDLE;
}

TEST_CASE("RmlUi saved layer texture preserves requested padded bounds")
{
    BgfxTargetCache target_cache;
    BgfxLayerSystem layer_system(target_cache);
    layer_system.begin_frame();

    LayerRecord& layer = target_cache.prepare_virtual_layer_slot(1);
    layer.framebuffer = bgfx::FrameBufferHandle{3};
    layer.color = bgfx::TextureHandle{4};
    layer.bounds = RenderBounds{{50.0f, 60.0f, 20.0f, 20.0f}, {50, 60, 20, 20}};
    layer.valid_content_bounds = {50, 60, 20, 20};
    layer.has_valid_content_bounds = true;
    layer.texture_width = 20;
    layer.texture_height = 20;
    layer.kind = LayerKind::VirtualChild;
    layer.materialized = true;

    layer_system.push_layer(1);

    std::unordered_map<Rml::TextureHandle, TextureRecord> textures;
    Rml::TextureHandle texture_counter = 10;
    bool old_copy_called = false;
    Rml::Rectanglei copied_region = Rml::Rectanglei::FromPositionSize({0, 0}, {0, 0});
    Rml::Vector2i copied_output_dimensions;
    Rml::Vector2i copied_destination_offset;

    BgfxLayerSaveTextureContext ctx;
    ctx.textures = &textures;
    ctx.texture_counter = &texture_counter;
    ctx.current_save_bounds = [] { return Rml::Rectanglei::FromPositionSize({10, 20}, {100, 80}); };
    ctx.materialize_layer = [](Rml::LayerHandle, std::optional<FbRect>) { return true; };
    ctx.copy_region_to_texture = [&](bgfx::TextureHandle, Rml::Rectanglei, int, int, const char*,
                                     bool) {
        old_copy_called = true;
        return bgfx::TextureHandle{8};
    };
    ctx.copy_region_to_sized_texture =
        [&](bgfx::TextureHandle, Rml::Rectanglei region, int, int, Rml::Vector2i output_dimensions,
            Rml::Vector2i destination_offset, const char*, bool flip_y) {
            copied_region = region;
            copied_output_dimensions = output_dimensions;
            copied_destination_offset = destination_offset;
            CHECK(flip_y);
            return bgfx::TextureHandle{9};
        };

    const Rml::TextureHandle texture = layer_system.save_layer_as_texture(ctx);

    REQUIRE(texture == 11);
    CHECK_FALSE(old_copy_called);
    CHECK(copied_region.Left() == 0);
    CHECK(copied_region.Top() == 0);
    CHECK(copied_region.Width() == 20);
    CHECK(copied_region.Height() == 20);
    CHECK(copied_output_dimensions.x == 100);
    CHECK(copied_output_dimensions.y == 80);
    CHECK(copied_destination_offset.x == 40);
    CHECK(copied_destination_offset.y == 40);
    REQUIRE(textures.contains(texture));
    CHECK(textures.at(texture).dimensions.x == 100);
    CHECK(textures.at(texture).dimensions.y == 80);

    layer.framebuffer = BGFX_INVALID_HANDLE;
}

TEST_CASE("RmlUi nested layer pop restores exact parent handles")
{
    BgfxTargetCache target_cache;
    BgfxLayerSystem layer_system(target_cache);
    layer_system.begin_frame();
    [[maybe_unused]] LayerRecord& reserved_child_slot = target_cache.prepare_virtual_layer_slot(2);

    LayerRecord& root = *layer_system.current_layer();
    root.clip_mask_enabled = true;
    root.stencil_ref = 3;
    root.conservative_mask_bounds = ConservativeMaskBounds{{10, 20, 80, 60}, true, false};
    root.clip_commands = {4, 8};

    const RenderBounds parent_bounds{{10.0f, 20.0f, 140.0f, 90.0f}, {10, 20, 140, 90}};
    const ScissorState parent_scissor{true, Rml::Rectanglei::FromPositionSize({12, 22}, {120, 70})};
    LayerRecord& parent =
        layer_system.prepare_virtual_child(1, 0, parent_bounds, parent_scissor, true);
    layer_system.push_layer(1);

    CHECK(layer_system.active_layer() == 1);
    CHECK(parent.parent_layer == 0);
    CHECK(parent.push_scissor.enabled);
    CHECK(parent.push_scissor.region.Left() == 12);
    CHECK(parent.push_transform_valid);
    CHECK(parent.clip_mask_enabled);
    CHECK(parent.stencil_ref == 3);
    CHECK(parent.inherited_clip_command_count == 2);

    const RenderBounds child_bounds{{18.0f, 28.0f, 60.0f, 40.0f}, {18, 28, 60, 40}};
    const ScissorState child_scissor{true, Rml::Rectanglei::FromPositionSize({18, 28}, {60, 40})};
    LayerRecord& child =
        layer_system.prepare_virtual_child(2, 1, child_bounds, child_scissor, false);
    layer_system.push_layer(2);

    CHECK(layer_system.active_layer() == 2);
    CHECK(child.parent_layer == 1);
    CHECK(child.push_scissor.enabled);
    CHECK(!child.push_transform_valid);
    CHECK(child.clip_mask_enabled);
    CHECK(child.stencil_ref == 3);
    CHECK(child.inherited_clip_command_count == 2);

    REQUIRE(layer_system.pop_layer());
    CHECK(layer_system.active_layer() == 1);
    CHECK(layer_system.current_layer() == &parent);
    CHECK(layer_system.current_layer()->push_scissor.enabled);
    CHECK(layer_system.current_layer()->push_transform_valid);
    CHECK(layer_system.current_layer()->clip_mask_enabled);
    CHECK(layer_system.current_layer()->stencil_ref == 3);

    REQUIRE(layer_system.pop_layer());
    CHECK(layer_system.active_layer() == 0);
    CHECK(layer_system.current_layer() == &root);
    CHECK(layer_system.current_layer()->clip_mask_enabled);
    CHECK(layer_system.current_layer()->stencil_ref == 3);

    CHECK(!layer_system.pop_layer());
    CHECK(layer_system.active_layer() == 0);
}
