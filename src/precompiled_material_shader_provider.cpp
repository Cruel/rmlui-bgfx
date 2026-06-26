#include <rmlui_bgfx/precompiled_material_shader_provider.hpp>

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/SystemInterface.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <utility>

namespace rmlui_bgfx {

namespace {

[[nodiscard]] std::string join_path(std::string_view root, std::string_view path)
{
    if (path.empty()) {
        return std::string(root);
    }
    if (path.size() >= 1 && (path[0] == '/' || path[0] == '\\')) {
        return std::string(path);
    }
    if (path.size() >= 2 && path[1] == ':') {
        return std::string(path);
    }
    if (root.empty()) {
        return std::string(path);
    }
    std::string result(root);
    if (result.back() != '/' && result.back() != '\\') {
        result.push_back('/');
    }
    result.append(path);
    return result;
}

[[nodiscard]] bool read_file(std::string_view path, std::vector<std::uint8_t>& out,
                             std::string* error_message)
{
    std::ifstream file(std::string(path), std::ios::binary);
    if (!file) {
        if (error_message) {
            *error_message = "failed to open shader binary: ";
            error_message->append(path);
        }
        return false;
    }

    file.unsetf(std::ios::skipws);
    file.seekg(0, std::ios::end);
    const std::streampos size = file.tellg();
    if (size <= 0) {
        if (error_message) {
            *error_message = "empty shader binary: ";
            error_message->append(path);
        }
        return false;
    }
    file.seekg(0, std::ios::beg);

    out.clear();
    out.reserve(static_cast<std::size_t>(size));
    out.insert(out.begin(), std::istream_iterator<std::uint8_t>(file),
               std::istream_iterator<std::uint8_t>());
    if (out.empty()) {
        if (error_message) {
            *error_message = "failed to read shader binary: ";
            error_message->append(path);
        }
        return false;
    }
    return true;
}

} // namespace

PrecompiledMaterialShaderProvider::PrecompiledMaterialShaderProvider(
    PrecompiledMaterialShaderProviderConfig config)
    : m_config(std::move(config))
{
    m_sampler =
        bgfx::createUniform(m_config.sampler_uniform_name.c_str(), bgfx::UniformType::Sampler);
    m_params0 = bgfx::createUniform(m_config.params0_uniform_name.c_str(), bgfx::UniformType::Vec4);
    m_params1 = bgfx::createUniform(m_config.params1_uniform_name.c_str(), bgfx::UniformType::Vec4);
}

PrecompiledMaterialShaderProvider::~PrecompiledMaterialShaderProvider()
{
    for (auto& [name, program] : m_programs) {
        (void)name;
        if (bgfx::isValid(program.program)) {
            bgfx::destroy(program.program);
        }
    }
    m_programs.clear();
    if (bgfx::isValid(m_sampler)) {
        bgfx::destroy(m_sampler);
        m_sampler = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(m_params0)) {
        bgfx::destroy(m_params0);
        m_params0 = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(m_params1)) {
        bgfx::destroy(m_params1);
        m_params1 = BGFX_INVALID_HANDLE;
    }
}

void PrecompiledMaterialShaderProvider::register_shader(std::string name, std::string program_stem)
{
    ShaderDefinition definition;
    definition.vertex_path = program_stem + m_config.vertex_suffix;
    definition.fragment_path = std::move(program_stem) + m_config.fragment_suffix;
    m_definitions[std::move(name)] = std::move(definition);
}

void PrecompiledMaterialShaderProvider::register_shader(std::string name, std::string vertex_path,
                                                        std::string fragment_path)
{
    m_definitions[std::move(name)] =
        ShaderDefinition{std::move(vertex_path), std::move(fragment_path)};
}

void PrecompiledMaterialShaderProvider::set_mouse_position(Rml::Vector2f position, bool valid)
{
    m_mouse_position = position;
    m_mouse_position_valid = valid;
}

void PrecompiledMaterialShaderProvider::clear_mouse_position()
{
    m_mouse_position = Rml::Vector2f(-1.0f, -1.0f);
    m_mouse_position_valid = false;
}

RmlUiMaterialShaderHandle PrecompiledMaterialShaderProvider::compile_decorator_shader(
    const RmlUiMaterialShaderRequest& request)
{
    const std::string name(request.value);
    if (name.empty()) {
        return {};
    }
    if (!ensure_program(name)) {
        return {};
    }

    const uint64_t id = m_next_handle++;
    m_active_shaders.emplace(id, ActiveShader{name});
    return RmlUiMaterialShaderHandle{id};
}

void PrecompiledMaterialShaderProvider::release_decorator_shader(RmlUiMaterialShaderHandle shader)
{
    m_active_shaders.erase(shader.id);
}

bool PrecompiledMaterialShaderProvider::submit_decorator_shader(
    RmlUiMaterialShaderHandle shader, const RmlUiMaterialShaderDrawContext& context)
{
    const auto active_it = m_active_shaders.find(shader.id);
    if (active_it == m_active_shaders.end()) {
        return false;
    }
    LoadedProgram* loaded = ensure_program(active_it->second.name);
    if (!loaded || !bgfx::isValid(loaded->program) || !bgfx::isValid(context.vertex_buffer) ||
        !bgfx::isValid(context.index_buffer) || context.index_count == 0 ||
        !bgfx::isValid(m_sampler)) {
        return false;
    }

    if (context.scissor_enabled) {
        if (context.local_scissor.Width() <= 0 || context.local_scissor.Height() <= 0) {
            return false;
        }
        bgfx::setScissor(
            uint16_t(context.local_scissor.Left()), uint16_t(context.local_scissor.Top()),
            uint16_t(context.local_scissor.Width()), uint16_t(context.local_scissor.Height()));
    }

    const float translate[4] = {context.translation.x, context.translation.y, 0.0f, 0.0f};
    float time = 0.0f;
    if (Rml::SystemInterface* system = Rml::GetSystemInterface()) {
        time = float(system->GetElapsedTime());
    }
    const float material_params0[4] = {time, context.paint_dimensions.x, context.paint_dimensions.y,
                                       context.dpi_scale};
    const float material_params1[4] = {m_mouse_position.x, m_mouse_position.y,
                                       m_mouse_position_valid ? 1.0f : 0.0f, 0.0f};
    bgfx::setVertexBuffer(0, context.vertex_buffer);
    bgfx::setIndexBuffer(context.index_buffer, 0, context.index_count);
    bgfx::setUniform(context.projection_uniform, context.projection);
    bgfx::setUniform(context.transform_uniform, context.transform);
    bgfx::setUniform(context.translate_uniform, translate);
    if (bgfx::isValid(m_params0)) {
        bgfx::setUniform(m_params0, material_params0);
    }
    if (bgfx::isValid(m_params1)) {
        bgfx::setUniform(m_params1, material_params1);
    }
    bgfx::setTexture(0, m_sampler,
                     bgfx::isValid(context.texture) ? context.texture : context.white_texture);
    bgfx::setState(context.premultiplied_blend_state |
                   (context.msaa_enabled ? BGFX_STATE_MSAA : 0));
    if (context.clip_mask_enabled) {
        bgfx::setStencil(context.stencil_state, context.stencil_state);
    }
    bgfx::submit(context.view, loaded->program);
    return true;
}

PrecompiledMaterialShaderProvider::LoadedProgram*
PrecompiledMaterialShaderProvider::ensure_program(std::string_view name)
{
    const std::string key(name);
    auto loaded_it = m_programs.find(key);
    if (loaded_it != m_programs.end()) {
        return bgfx::isValid(loaded_it->second.program) ? &loaded_it->second : nullptr;
    }

    auto definition_it = m_definitions.find(key);
    if (definition_it == m_definitions.end()) {
        warning("no precompiled material shader registered for requested shader() value");
        return nullptr;
    }

    const std::string vertex_path =
        join_path(m_config.root_directory, definition_it->second.vertex_path);
    const std::string fragment_path =
        join_path(m_config.root_directory, definition_it->second.fragment_path);
    bgfx::ShaderHandle vertex = load_shader(vertex_path);
    bgfx::ShaderHandle fragment = load_shader(fragment_path);
    if (!bgfx::isValid(vertex) || !bgfx::isValid(fragment)) {
        if (bgfx::isValid(vertex)) {
            bgfx::destroy(vertex);
        }
        if (bgfx::isValid(fragment)) {
            bgfx::destroy(fragment);
        }
        return nullptr;
    }

    bgfx::ProgramHandle program = bgfx::createProgram(vertex, fragment, true);
    if (!bgfx::isValid(program)) {
        error("failed to create precompiled material shader program");
        bgfx::destroy(vertex);
        bgfx::destroy(fragment);
        return nullptr;
    }

    LoadedProgram loaded;
    loaded.program = program;
    auto [inserted_it, inserted] = m_programs.emplace(key, loaded);
    (void)inserted;
    return &inserted_it->second;
}

bgfx::ShaderHandle PrecompiledMaterialShaderProvider::load_shader(std::string_view path)
{
    std::vector<std::uint8_t> bytes;
    std::string message;
    if (!load_binary(path, bytes, &message)) {
        error(message.empty() ? "failed to load precompiled material shader binary" : message);
        return BGFX_INVALID_HANDLE;
    }
    bgfx::ShaderHandle shader =
        bgfx::createShader(bgfx::copy(bytes.data(), static_cast<std::uint32_t>(bytes.size())));
    if (!bgfx::isValid(shader)) {
        error("failed to create precompiled material shader from binary");
        return BGFX_INVALID_HANDLE;
    }
    const std::string name(path);
    bgfx::setName(shader, name.c_str());
    return shader;
}

bool PrecompiledMaterialShaderProvider::load_binary(std::string_view path,
                                                    std::vector<std::uint8_t>& out,
                                                    std::string* error_message) const
{
    if (m_config.load_binary) {
        return m_config.load_binary(path, out, error_message);
    }
    return read_file(path, out, error_message);
}

void PrecompiledMaterialShaderProvider::warning(std::string_view message) const
{
    if (m_config.diagnostics) {
        m_config.diagnostics->warning(message);
    } else {
        std::fprintf(stderr, "[rmlui-bgfx] warning: %.*s\n", int(message.size()), message.data());
    }
}

void PrecompiledMaterialShaderProvider::error(std::string_view message) const
{
    if (m_config.diagnostics) {
        m_config.diagnostics->error(message);
    } else {
        std::fprintf(stderr, "[rmlui-bgfx] error: %.*s\n", int(message.size()), message.data());
    }
}

} // namespace rmlui_bgfx
