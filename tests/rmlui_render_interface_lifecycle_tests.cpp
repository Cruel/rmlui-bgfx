#include <rmlui_bgfx/render_interface.hpp>

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
