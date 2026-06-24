#include "rmlui_bgfx_layers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <unordered_map>
#include <vector>

using namespace rmlui_bgfx;

TEST_CASE("RmlUi saved mask image uses valid content bounds")
{
    BgfxTargetCache target_cache;
    BgfxLayerSystem layer_system(target_cache);
    layer_system.begin_frame();

    LayerRecord& layer = target_cache.prepare_virtual_layer_slot(1);
    layer.framebuffer = bgfx::FrameBufferHandle{3};
    layer.color = bgfx::TextureHandle{4};
    layer.bounds = RenderBounds{{10.0f, 20.0f, 100.0f, 100.0f}, {10, 20, 100, 100}};
    layer.valid_content_bounds = {30, 40, 20, 15};
    layer.has_valid_content_bounds = true;
    layer.texture_width = 100;
    layer.texture_height = 100;
    layer.kind = LayerKind::VirtualChild;
    layer.materialized = true;

    layer_system.push_layer(1);

    std::unordered_map<Rml::TextureHandle, TextureRecord> textures;
    std::unordered_map<Rml::CompiledFilterHandle, FilterRecord> filters;
    Rml::TextureHandle texture_counter = 7;
    Rml::CompiledFilterHandle filter_counter = 11;
    Rml::Rectanglei copied_region = Rml::Rectanglei::FromPositionSize({0, 0}, {0, 0});
    int copied_source_width = 0;
    int copied_source_height = 0;
    const char* copy_name = nullptr;

    BgfxLayerSaveMaskContext ctx;
    ctx.surface = SurfaceMetrics{200, 200, 200, 200, 1.0f, 1.0f};
    ctx.textures = &textures;
    ctx.filters = &filters;
    ctx.texture_counter = &texture_counter;
    ctx.filter_counter = &filter_counter;
    ctx.materialize_layer = [](Rml::LayerHandle, std::optional<FbRect>) { return true; };
    ctx.copy_region_to_texture = [&](bgfx::TextureHandle, Rml::Rectanglei region, int source_width,
                                     int source_height, const char* name) {
        copied_region = region;
        copied_source_width = source_width;
        copied_source_height = source_height;
        copy_name = name;
        return bgfx::TextureHandle{9};
    };

    const Rml::CompiledFilterHandle filter = layer_system.save_layer_as_mask_image(ctx);

    CHECK(filter == 12);
    REQUIRE(filters.contains(filter));
    REQUIRE(textures.contains(8));
    CHECK(copied_region.Left() == 20);
    CHECK(copied_region.Top() == 20);
    CHECK(copied_region.Width() == 20);
    CHECK(copied_region.Height() == 15);
    CHECK(copied_source_width == 100);
    CHECK(copied_source_height == 100);
    REQUIRE(copy_name != nullptr);
    CHECK(std::string_view(copy_name) == "RmlUi.SaveLayerAsMaskImage");

    const TextureRecord& texture = textures.at(8);
    CHECK(texture.dimensions.x == 20);
    CHECK(texture.dimensions.y == 15);
    CHECK(texture.bounds.framebuffer.x == 30);
    CHECK(texture.bounds.framebuffer.y == 40);
    CHECK(texture.bounds.framebuffer.w == 20);
    CHECK(texture.bounds.framebuffer.h == 15);

    const FilterRecord& record = filters.at(filter);
    CHECK(record.kind == FilterKind::MaskImage);
    CHECK(record.resource == 8);
    CHECK(record.mask_bounds[0] == 30);
    CHECK(record.mask_bounds[1] == 40);
    CHECK(record.mask_bounds[2] == 20);
    CHECK(record.mask_bounds[3] == 15);

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
