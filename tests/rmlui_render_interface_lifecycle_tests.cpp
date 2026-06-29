#include <rmlui_bgfx/render_interface.hpp>

#include "rmlui_bgfx_target_cache.hpp"

#include <RmlUi/Core/Dictionary.h>
#include <RmlUi/Core/Variant.h>
#include <bgfx/bgfx.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

class BgfxNoopScope {
public:
    BgfxNoopScope()
    {
        bgfx::Init init;
        init.type = bgfx::RendererType::Noop;
        init.resolution.width = 64;
        init.resolution.height = 64;
        init.resolution.reset = BGFX_RESET_NONE;
        m_initialized = bgfx::init(init);
    }

    ~BgfxNoopScope()
    {
        if (m_initialized) {
            bgfx::shutdown();
        }
    }

    [[nodiscard]] bool initialized() const noexcept { return m_initialized; }

private:
    bool m_initialized = false;
};

class NullShaderProvider final : public rmlui_bgfx::ShaderProvider {
public:
    [[nodiscard]] bgfx::ProgramHandle load_program(rmlui_bgfx::SystemProgram) override
    {
        return BGFX_INVALID_HANDLE;
    }
};

class NullTextureLoader final : public rmlui_bgfx::TextureLoader {
public:
    [[nodiscard]] bool load_rgba8(const char*, rmlui_bgfx::LoadedTexture&, std::string*) override
    {
        return false;
    }
};

class CapturingDiagnostics final : public rmlui_bgfx::Diagnostics {
public:
    void warning(std::string_view message) override { warnings.emplace_back(message); }
    void error(std::string_view message) override { errors.emplace_back(message); }

    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};

class CountingMaterialShaderProvider final : public rmlui_bgfx::MaterialShaderProvider {
public:
    [[nodiscard]] rmlui_bgfx::RmlUiMaterialShaderHandle
    compile_decorator_shader(const rmlui_bgfx::RmlUiMaterialShaderRequest& request) override
    {
        ++compile_count;
        last_value = std::string(request.value);
        return rmlui_bgfx::RmlUiMaterialShaderHandle{++next_id};
    }

    void release_decorator_shader(rmlui_bgfx::RmlUiMaterialShaderHandle shader) override
    {
        released_ids.push_back(shader.id);
    }

    [[nodiscard]] bool
    submit_decorator_shader(rmlui_bgfx::RmlUiMaterialShaderHandle,
                            const rmlui_bgfx::RmlUiMaterialShaderDrawContext&) override
    {
        return false;
    }

    int compile_count = 0;
    uint64_t next_id = 0;
    std::string last_value;
    std::vector<uint64_t> released_ids;
};

[[nodiscard]] Rml::CompiledGeometryHandle make_quad(rmlui_bgfx::RenderInterface& renderer)
{
    std::array<Rml::Vertex, 4> vertices{};
    vertices[0].position = {0.0f, 0.0f};
    vertices[1].position = {8.0f, 0.0f};
    vertices[2].position = {8.0f, 8.0f};
    vertices[3].position = {0.0f, 8.0f};
    for (Rml::Vertex& vertex : vertices) {
        vertex.colour = Rml::ColourbPremultiplied(255, 255, 255, 255);
    }
    const std::array<int, 6> indices{0, 1, 2, 0, 2, 3};
    return renderer.CompileGeometry(Rml::Span<const Rml::Vertex>(vertices.data(), vertices.size()),
                                    Rml::Span<const int>(indices.data(), indices.size()));
}

} // namespace

TEST_CASE("RmlUi target cache layer metadata generations are stable across compatible reuse")
{
    BgfxNoopScope bgfx;
    REQUIRE(bgfx.initialized());

    rmlui_bgfx::BgfxTargetCache target_cache;
    const rmlui_bgfx::RenderBounds first_bounds{{0.0f, 0.0f, 16.0f, 16.0f}, {0, 0, 16, 16}};
    REQUIRE(target_cache.ensure_layer_target(1, first_bounds, bgfx::TextureFormat::D24S8, 0));

    const rmlui_bgfx::LayerRecord* first = target_cache.layer(1);
    REQUIRE(first != nullptr);
    CHECK(first->target_lifetime == rmlui_bgfx::TargetLifetime::Viewport);
    CHECK(first->target_generation != 0);
    CHECK(first->color_format == bgfx::TextureFormat::RGBA8);
    CHECK(first->depth_stencil_format == bgfx::TextureFormat::D24S8);
    CHECK(first->texture_width == 16);
    CHECK(first->texture_height == 16);
    const uint64_t first_generation = first->target_generation;

    const rmlui_bgfx::RenderBounds moved_bounds{{4.0f, 8.0f, 16.0f, 16.0f}, {4, 8, 16, 16}};
    REQUIRE(target_cache.ensure_layer_target(1, moved_bounds, bgfx::TextureFormat::D24S8, 0));

    const rmlui_bgfx::LayerRecord* reused = target_cache.layer(1);
    REQUIRE(reused != nullptr);
    CHECK(reused->target_generation == first_generation);
    CHECK(reused->bounds.framebuffer.x == 4);
    CHECK(reused->bounds.framebuffer.y == 8);

    target_cache.resize({64, 64, 64, 64, 1.0f, 1.0f});
    REQUIRE(target_cache.ensure_layer_target(1, first_bounds, bgfx::TextureFormat::D24S8, 0));
    const rmlui_bgfx::LayerRecord* recreated = target_cache.layer(1);
    REQUIRE(recreated != nullptr);
    CHECK(recreated->target_generation > first_generation);
}

TEST_CASE("RmlUi target cache postprocess lifetimes split viewport and frame targets")
{
    BgfxNoopScope bgfx;
    REQUIRE(bgfx.initialized());

    rmlui_bgfx::BgfxTargetCache target_cache;
    const rmlui_bgfx::SurfaceMetrics surface{64, 64, 64, 64, 1.0f, 1.0f};
    const rmlui_bgfx::FbRect bounded_bounds{4, 8, 20, 12};

    rmlui_bgfx::RenderTargetRecord* bounded =
        target_cache.acquire_postprocess_target(rmlui_bgfx::PostprocessTargetKind::Secondary,
                                                bounded_bounds, surface);
    REQUIRE(bounded != nullptr);
    CHECK(bounded->kind == rmlui_bgfx::PostprocessTargetKind::Secondary);
    CHECK(bounded->lifetime == rmlui_bgfx::TargetLifetime::Frame);
    CHECK_FALSE(bounded->full_frame);
    CHECK(bounded->generation != 0);
    CHECK(bounded->bounds.x == 4);
    CHECK(bounded->bounds.y == 8);
    CHECK(bounded->texture_width == 20);
    CHECK(bounded->texture_height == 12);
    const uint64_t bounded_generation = bounded->generation;

    rmlui_bgfx::RenderTargetRecord* bounded_reused =
        target_cache.acquire_postprocess_target(rmlui_bgfx::PostprocessTargetKind::Secondary,
                                                {10, 16, 20, 12}, surface);
    REQUIRE(bounded_reused != nullptr);
    CHECK(bounded_reused->generation == bounded_generation);
    CHECK(bounded_reused->bounds.x == 10);
    CHECK(bounded_reused->bounds.y == 16);

    rmlui_bgfx::RenderTargetRecord* full_frame =
        target_cache.acquire_postprocess_target(rmlui_bgfx::PostprocessTargetKind::Primary,
                                                {0, 0, 64, 64}, surface);
    REQUIRE(full_frame != nullptr);
    CHECK(full_frame->lifetime == rmlui_bgfx::TargetLifetime::Viewport);
    CHECK(full_frame->full_frame);
    CHECK(full_frame->surface_width == 64);
    CHECK(full_frame->surface_height == 64);
    CHECK(full_frame->first_used_frame == 0);
    CHECK(full_frame->last_used_frame == 0);
    const uint64_t full_frame_generation = full_frame->generation;

    rmlui_bgfx::RenderTargetRecord* full_frame_reused_same_frame =
        target_cache.acquire_postprocess_target(rmlui_bgfx::PostprocessTargetKind::Primary,
                                                {0, 0, 64, 64}, surface);
    REQUIRE(full_frame_reused_same_frame != nullptr);
    CHECK(full_frame_reused_same_frame->generation == full_frame_generation);
    CHECK(full_frame_reused_same_frame->last_used_frame == 0);

    target_cache.begin_frame();
    REQUIRE(target_cache.postprocess_targets().size() == 1);
    CHECK(target_cache.postprocess_targets().front().generation == full_frame_generation);

    rmlui_bgfx::RenderTargetRecord* full_frame_next =
        target_cache.acquire_postprocess_target(rmlui_bgfx::PostprocessTargetKind::Primary,
                                                {0, 0, 64, 64}, surface);
    REQUIRE(full_frame_next != nullptr);
    CHECK(full_frame_next->generation == full_frame_generation);
    CHECK(full_frame_next->lifetime == rmlui_bgfx::TargetLifetime::Viewport);
    CHECK(full_frame_next->first_used_frame == 0);
    CHECK(full_frame_next->last_used_frame == 1);

    rmlui_bgfx::RenderTargetRecord* bounded_next =
        target_cache.acquire_postprocess_target(rmlui_bgfx::PostprocessTargetKind::Secondary,
                                                bounded_bounds, surface);
    REQUIRE(bounded_next != nullptr);
    CHECK(bounded_next->generation > full_frame_generation);
    CHECK(bounded_next->generation > bounded_generation);
    CHECK(bounded_next->lifetime == rmlui_bgfx::TargetLifetime::Frame);

    target_cache.resize(surface);
    rmlui_bgfx::RenderTargetRecord* recreated_full_frame =
        target_cache.acquire_postprocess_target(rmlui_bgfx::PostprocessTargetKind::Primary,
                                                {0, 0, 64, 64}, surface);
    REQUIRE(recreated_full_frame != nullptr);
    CHECK(recreated_full_frame->generation > full_frame_generation);
}

TEST_CASE("RmlUi target cache drops bounded postprocess targets at frame boundary")
{
    BgfxNoopScope bgfx;
    REQUIRE(bgfx.initialized());

    rmlui_bgfx::BgfxTargetCache target_cache;
    const rmlui_bgfx::SurfaceMetrics surface{128, 128, 128, 128, 1.0f, 1.0f};

    REQUIRE(target_cache.acquire_postprocess_target(rmlui_bgfx::PostprocessTargetKind::Primary,
                                                    {0, 0, 128, 128}, surface) != nullptr);
    REQUIRE(target_cache.acquire_postprocess_target(rmlui_bgfx::PostprocessTargetKind::Secondary,
                                                    {0, 0, 16, 16}, surface) != nullptr);
    REQUIRE(target_cache.acquire_postprocess_target(rmlui_bgfx::PostprocessTargetKind::Tertiary,
                                                    {10, 12, 32, 24}, surface) != nullptr);
    CHECK(target_cache.postprocess_targets().size() == 3);

    target_cache.begin_frame();
    REQUIRE(target_cache.postprocess_targets().size() == 1);
    CHECK(target_cache.postprocess_targets().front().lifetime ==
          rmlui_bgfx::TargetLifetime::Viewport);
    CHECK(target_cache.postprocess_targets().front().kind ==
          rmlui_bgfx::PostprocessTargetKind::Primary);
}

TEST_CASE("RmlUi target cache keeps postprocess roles isolated at matching full-frame size")
{
    BgfxNoopScope bgfx;
    REQUIRE(bgfx.initialized());

    rmlui_bgfx::BgfxTargetCache target_cache;
    const rmlui_bgfx::SurfaceMetrics surface{64, 64, 64, 64, 1.0f, 1.0f};

    rmlui_bgfx::RenderTargetRecord* primary =
        target_cache.acquire_postprocess_target(rmlui_bgfx::PostprocessTargetKind::Primary,
                                                {0, 0, 64, 64}, surface);
    rmlui_bgfx::RenderTargetRecord* secondary =
        target_cache.acquire_postprocess_target(rmlui_bgfx::PostprocessTargetKind::Secondary,
                                                {0, 0, 64, 64}, surface);
    REQUIRE(primary != nullptr);
    REQUIRE(secondary != nullptr);
    CHECK(primary->generation != secondary->generation);
    CHECK(primary->kind == rmlui_bgfx::PostprocessTargetKind::Primary);
    CHECK(secondary->kind == rmlui_bgfx::PostprocessTargetKind::Secondary);
    CHECK(primary->lifetime == rmlui_bgfx::TargetLifetime::Viewport);
    CHECK(secondary->lifetime == rmlui_bgfx::TargetLifetime::Viewport);
    CHECK(target_cache.postprocess_targets().size() == 2);

    target_cache.begin_frame();
    CHECK(target_cache.postprocess_targets().size() == 2);
}

TEST_CASE("RmlUi bgfx render interface release paths tolerate stale handles")
{
    BgfxNoopScope bgfx;
    REQUIRE(bgfx.initialized());

    NullShaderProvider shaders;
    NullTextureLoader textures;
    CapturingDiagnostics diagnostics;
    CountingMaterialShaderProvider material_shaders;

    rmlui_bgfx::RendererConfig config;
    config.surface = rmlui_bgfx::SurfaceMetrics{64, 64, 64, 64, 1.0f, 1.0f};
    config.views = rmlui_bgfx::ViewRange{0, 32};
    config.shaders = &shaders;
    config.textures = &textures;
    config.diagnostics = &diagnostics;
    config.material_shaders = &material_shaders;

    rmlui_bgfx::RenderInterface renderer(config);

    const Rml::CompiledGeometryHandle geometry = make_quad(renderer);
    REQUIRE(geometry != 0);
    renderer.ReleaseGeometry(geometry);
    renderer.ReleaseGeometry(geometry);
    renderer.RenderGeometry(geometry, {0.0f, 0.0f}, 0);

    const std::array<Rml::byte, 4> pixel{255, 255, 255, 255};
    const Rml::TextureHandle texture =
        renderer.GenerateTexture(Rml::Span<const Rml::byte>(pixel.data(), pixel.size()), {1, 1});
    REQUIRE(texture != 0);
    renderer.ReleaseTexture(texture);
    renderer.ReleaseTexture(texture);

    Rml::Dictionary filter_parameters;
    filter_parameters["sigma"] = Rml::Variant(2.0f);
    const Rml::CompiledFilterHandle filter = renderer.CompileFilter("blur", filter_parameters);
    REQUIRE(filter != 0);
    renderer.ReleaseFilter(filter);
    renderer.ReleaseFilter(filter);

    Rml::Dictionary no_op_filter_parameters;
    no_op_filter_parameters["value"] = Rml::Variant(1.0f);
    const Rml::CompiledFilterHandle no_op_filter =
        renderer.CompileFilter("opacity", no_op_filter_parameters);
    REQUIRE(no_op_filter != 0);
    renderer.ReleaseFilter(no_op_filter);

    Rml::Dictionary shader_parameters;
    shader_parameters["value"] = Rml::Variant(Rml::String("ui/noise_panel"));
    shader_parameters["dimensions"] = Rml::Variant(Rml::Vector2f(48.0f, 32.0f));
    const Rml::CompiledShaderHandle shader = renderer.CompileShader("shader", shader_parameters);
    REQUIRE(shader != 0);
    CHECK(material_shaders.compile_count == 1);
    CHECK(material_shaders.last_value == "ui/noise_panel");

    renderer.ReleaseShader(shader);
    renderer.ReleaseShader(shader);
    renderer.RenderShader(shader, geometry, {0.0f, 0.0f}, 0);

    REQUIRE(material_shaders.released_ids.size() == 1);
    CHECK(material_shaders.released_ids[0] == 1);
}
