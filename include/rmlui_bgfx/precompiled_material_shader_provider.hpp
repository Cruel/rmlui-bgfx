#pragma once

#include <rmlui_bgfx/config.hpp>

#include <bgfx/bgfx.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rmlui_bgfx {

struct PrecompiledMaterialShaderProviderConfig {
    using LoadBinaryFn = std::function<bool(std::string_view path, std::vector<std::uint8_t>& out,
                                            std::string* error_message)>;

    std::string root_directory;
    std::string vertex_suffix = ".vs.bin";
    std::string fragment_suffix = ".fs.bin";
    std::string sampler_uniform_name = "s_texColor";
    std::string params0_uniform_name = "u_rmluiMaterialParams0";
    std::string params1_uniform_name = "u_rmluiMaterialParams1";
    LoadBinaryFn load_binary;
    Diagnostics* diagnostics = nullptr;
};

class PrecompiledMaterialShaderProvider final : public MaterialShaderProvider {
public:
    explicit PrecompiledMaterialShaderProvider(PrecompiledMaterialShaderProviderConfig config);
    ~PrecompiledMaterialShaderProvider() override;

    PrecompiledMaterialShaderProvider(const PrecompiledMaterialShaderProvider&) = delete;
    PrecompiledMaterialShaderProvider& operator=(const PrecompiledMaterialShaderProvider&) = delete;

    void register_shader(std::string name, std::string program_stem);
    void register_shader(std::string name, std::string vertex_path, std::string fragment_path);

    void set_mouse_position(Rml::Vector2f position, bool valid = true);
    void clear_mouse_position();

    [[nodiscard]] RmlUiMaterialShaderHandle
    compile_decorator_shader(const RmlUiMaterialShaderRequest& request) override;
    void release_decorator_shader(RmlUiMaterialShaderHandle shader) override;
    [[nodiscard]] bool
    submit_decorator_shader(RmlUiMaterialShaderHandle shader,
                            const RmlUiMaterialShaderDrawContext& context) override;

private:
    struct ShaderDefinition {
        std::string vertex_path;
        std::string fragment_path;
    };

    struct LoadedProgram {
        bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
    };

    struct ActiveShader {
        std::string name;
    };

    [[nodiscard]] LoadedProgram* ensure_program(std::string_view name);
    [[nodiscard]] bgfx::ShaderHandle load_shader(std::string_view path);
    [[nodiscard]] bool load_binary(std::string_view path, std::vector<std::uint8_t>& out,
                                   std::string* error_message) const;
    void warning(std::string_view message) const;
    void error(std::string_view message) const;

    PrecompiledMaterialShaderProviderConfig m_config;
    bgfx::UniformHandle m_sampler = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_params0 = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_params1 = BGFX_INVALID_HANDLE;
    uint64_t m_next_handle = 1;
    Rml::Vector2f m_mouse_position = Rml::Vector2f(-1.0f, -1.0f);
    bool m_mouse_position_valid = false;
    std::unordered_map<std::string, ShaderDefinition> m_definitions;
    std::unordered_map<std::string, LoadedProgram> m_programs;
    std::unordered_map<uint64_t, ActiveShader> m_active_shaders;
};

} // namespace rmlui_bgfx
