#include "RmlUi_Backend.h"
#include "RmlUi_Platform_SDL.h"

#include <rmlui_bgfx/precompiled_material_shader_provider.hpp>
#include <rmlui_bgfx/render_interface.hpp>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/Log.h>
#include <RmlUi/Core/Math.h>
#include <RmlUi/Core/Profiling.h>

#include <SDL3/SDL.h>

#include <bgfx/bgfx.h>
#include <bgfx/platform.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include <stb_image.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef RMLUI_BGFX_SAMPLE_SHADER_DIR
#error "RMLUI_BGFX_SAMPLE_SHADER_DIR must point to compiled rmlui-bgfx shader binaries."
#endif

namespace {

[[nodiscard]] const char* shader_stem(rmlui_bgfx::SystemProgram program)
{
    switch (program) {
    case rmlui_bgfx::SystemProgram::RmlUi:
        return "rmlui";
    case rmlui_bgfx::SystemProgram::Composite:
        return "rmlui_composite";
    case rmlui_bgfx::SystemProgram::CompositeFilter:
        return "rmlui_composite_filter";
    case rmlui_bgfx::SystemProgram::Copy:
        return "rmlui_copy";
    case rmlui_bgfx::SystemProgram::Opacity:
        return "rmlui_opacity";
    case rmlui_bgfx::SystemProgram::ColorMatrix:
        return "rmlui_color_matrix";
    case rmlui_bgfx::SystemProgram::MaskMultiply:
        return "rmlui_mask_multiply";
    case rmlui_bgfx::SystemProgram::Blur:
        return "rmlui_blur";
    case rmlui_bgfx::SystemProgram::DropShadow:
        return "rmlui_drop_shadow";
    case rmlui_bgfx::SystemProgram::Gradient:
        return "rmlui_gradient";
    }
    return "unknown";
}

[[nodiscard]] rmlui_bgfx::RenderPath render_path_from_env()
{
    const char* value = std::getenv("RMLUI_BGFX_RENDER_PATH");
    if (!value) {
        return rmlui_bgfx::RenderPath::Optimized;
    }
    const std::string_view mode(value);
    if (mode == "optimized" || mode == "bounded" || mode == "fast") {
        return rmlui_bgfx::RenderPath::Optimized;
    }
    if (mode == "reference" || mode == "ref" || mode == "gl3" || mode == "gl3-compatible" ||
        mode == "compatible" || mode == "compat") {
        return rmlui_bgfx::RenderPath::Reference;
    }
    std::fprintf(stderr, "[rmlui-bgfx] unknown RMLUI_BGFX_RENDER_PATH='%s'; using optimized\n",
                 value);
    return rmlui_bgfx::RenderPath::Optimized;
}

[[nodiscard]] rmlui_bgfx::BlurSampleBoundsMode blur_sample_bounds_mode_from_env()
{
    const char* value = std::getenv("RMLUI_BGFX_BLUR_SAMPLE_BOUNDS");
    if (!value) {
        return rmlui_bgfx::BlurSampleBoundsMode::SourceBounds;
    }
    const std::string_view mode(value);
    if (mode == "full" || mode == "full-texture" || mode == "texture") {
        return rmlui_bgfx::BlurSampleBoundsMode::FullTexture;
    }
    return rmlui_bgfx::BlurSampleBoundsMode::SourceBounds;
}

[[nodiscard]] bool env_flag_enabled(const char* name)
{
    const char* value = std::getenv(name);
    return value && (value[0] == '1' || value[0] == 't' || value[0] == 'T' || value[0] == 'y' ||
                     value[0] == 'Y' || value[0] == 'o' || value[0] == 'O');
}

[[nodiscard]] bool trace_filter_pipeline_from_env()
{
    return env_flag_enabled("RMLUI_BGFX_FILTER_TRACE");
}

[[nodiscard]] bool bounded_transform_layers_from_env()
{
    return env_flag_enabled("RMLUI_BGFX_BOUNDED_TRANSFORM_LAYERS");
}

[[nodiscard]] uint8_t reference_msaa_samples_from_env()
{
    const char* value = std::getenv("RMLUI_BGFX_REFERENCE_MSAA");
    if (!value || value[0] == '\0') {
        return 2;
    }
    const int samples = std::atoi(value);
    switch (samples) {
    case 0:
    case 2:
    case 4:
    case 8:
    case 16:
        return uint8_t(samples);
    default:
        std::fprintf(stderr, "[rmlui-bgfx] unknown RMLUI_BGFX_REFERENCE_MSAA='%s'; using 2\n",
                     value);
        return 2;
    }
}

[[nodiscard]] std::vector<std::uint8_t> read_file(const std::string& path)
{
    SDL_IOStream* io = SDL_IOFromFile(path.c_str(), "rb");
    if (!io)
        return {};

    const Sint64 size = SDL_GetIOSize(io);
    if (size <= 0) {
        SDL_CloseIO(io);
        return {};
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    const std::size_t read = SDL_ReadIO(io, bytes.data(), bytes.size());
    SDL_CloseIO(io);

    if (read != bytes.size())
        return {};
    return bytes;
}

[[nodiscard]] bgfx::ShaderHandle load_shader_binary(const std::string& path)
{
    std::vector<std::uint8_t> bytes = read_file(path);
    if (bytes.empty()) {
        Rml::Log::Message(Rml::Log::LT_ERROR, "Failed to read bgfx shader binary: %s",
                          path.c_str());
        return BGFX_INVALID_HANDLE;
    }

    bgfx::ShaderHandle shader =
        bgfx::createShader(bgfx::copy(bytes.data(), static_cast<std::uint32_t>(bytes.size())));
    if (!bgfx::isValid(shader)) {
        Rml::Log::Message(Rml::Log::LT_ERROR, "Failed to create bgfx shader from binary: %s",
                          path.c_str());
        return BGFX_INVALID_HANDLE;
    }

    bgfx::setName(shader, path.c_str());
    return shader;
}

class SampleBgfxCallback final : public bgfx::CallbackI {
public:
    explicit SampleBgfxCallback(bool in_trace_enabled) : trace_enabled(in_trace_enabled) {}

    void fatal(const char* file_path, uint16_t line, bgfx::Fatal::Enum,
               const char* message) override
    {
        std::fprintf(stderr, "%s (%u): BGFX fatal: %s\n", file_path ? file_path : "<unknown>",
                     unsigned(line), message ? message : "<no message>");
        std::abort();
    }

    void traceVargs(const char* file_path, uint16_t line, const char* format, va_list args) override
    {
        if (!trace_enabled) {
            return;
        }
        std::fprintf(stderr, "%s (%u): BGFX ", file_path ? file_path : "<unknown>", unsigned(line));
        std::vfprintf(stderr, format ? format : "", args);
    }

    void profilerBegin(const char*, uint32_t, const char*, uint16_t) override {}
    void profilerBeginLiteral(const char*, uint32_t, const char*, uint16_t) override {}
    void profilerEnd() override {}
    uint32_t cacheReadSize(uint64_t) override { return 0; }
    bool cacheRead(uint64_t, void*, uint32_t) override { return false; }
    void cacheWrite(uint64_t, const void*, uint32_t) override {}
    void screenShot(const char*, uint32_t, uint32_t, uint32_t, const void*, uint32_t, bool) override
    {
    }
    void captureBegin(uint32_t, uint32_t, uint32_t, bgfx::TextureFormat::Enum, bool) override {}
    void captureEnd() override {}
    void captureFrame(const void*, uint32_t) override {}

private:
    bool trace_enabled = false;
};

class SampleShaderProvider final : public rmlui_bgfx::ShaderProvider {
public:
    [[nodiscard]] bgfx::ProgramHandle load_program(rmlui_bgfx::SystemProgram program) override
    {
        const std::string stem = shader_stem(program);
        const std::string base = std::string(RMLUI_BGFX_SAMPLE_SHADER_DIR) + "/" + stem;
        bgfx::ShaderHandle vertex = load_shader_binary(base + ".vs.bin");
        bgfx::ShaderHandle fragment = load_shader_binary(base + ".fs.bin");
        if (!bgfx::isValid(vertex) || !bgfx::isValid(fragment)) {
            if (bgfx::isValid(vertex))
                bgfx::destroy(vertex);
            if (bgfx::isValid(fragment))
                bgfx::destroy(fragment);
            return BGFX_INVALID_HANDLE;
        }

        bgfx::ProgramHandle program_handle = bgfx::createProgram(vertex, fragment, true);
        if (!bgfx::isValid(program_handle))
            Rml::Log::Message(Rml::Log::LT_ERROR, "Failed to create bgfx program for %s",
                              stem.c_str());
        return program_handle;
    }
};

class SampleTextureLoader final : public rmlui_bgfx::TextureLoader {
public:
    [[nodiscard]] bool load_rgba8(const char* source, rmlui_bgfx::LoadedTexture& out,
                                  std::string* error_message) override
    {
        Rml::FileInterface* file_interface = Rml::GetFileInterface();
        if (!file_interface) {
            set_error(error_message, "RmlUi file interface is not available");
            return false;
        }

        Rml::FileHandle file = file_interface->Open(source);
        if (!file) {
            set_error(error_message, std::string("Failed to open texture: ") + source);
            return false;
        }

        file_interface->Seek(file, 0, SEEK_END);
        const std::size_t size = file_interface->Tell(file);
        file_interface->Seek(file, 0, SEEK_SET);

        std::vector<std::uint8_t> bytes(size);
        const std::size_t read = file_interface->Read(bytes.data(), size, file);
        file_interface->Close(file);
        if (read != size) {
            set_error(error_message, std::string("Failed to read complete texture: ") + source);
            return false;
        }

        int width = 0;
        int height = 0;
        int components = 0;
        stbi_uc* pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                                &width, &height, &components, 4);
        if (!pixels || width <= 0 || height <= 0) {
            set_error(error_message, std::string("stb_image failed for texture: ") + source);
            if (pixels)
                stbi_image_free(pixels);
            return false;
        }

        const std::size_t pixel_size =
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
        out.width = width;
        out.height = height;
        out.rgba8.assign(pixels, pixels + pixel_size);
        stbi_image_free(pixels);
        return true;
    }

private:
    static void set_error(std::string* error_message, std::string message)
    {
        if (error_message)
            *error_message = std::move(message);
    }
};

class SampleDiagnostics final : public rmlui_bgfx::Diagnostics {
public:
    void warning(std::string_view message) override
    {
        Rml::Log::Message(Rml::Log::LT_WARNING, "%.*s", int(message.size()), message.data());
    }

    void error(std::string_view message) override
    {
        Rml::Log::Message(Rml::Log::LT_ERROR, "%.*s", int(message.size()), message.data());
    }
};

[[nodiscard]] rmlui_bgfx::SurfaceMetrics query_surface(SDL_Window* window)
{
    int logical_width = 1;
    int logical_height = 1;
    int framebuffer_width = 1;
    int framebuffer_height = 1;

    SDL_GetWindowSize(window, &logical_width, &logical_height);
    SDL_GetWindowSizeInPixels(window, &framebuffer_width, &framebuffer_height);

    rmlui_bgfx::SurfaceMetrics metrics;
    metrics.logical_width = std::max(logical_width, 1);
    metrics.logical_height = std::max(logical_height, 1);
    metrics.framebuffer_width = std::max(framebuffer_width, 1);
    metrics.framebuffer_height = std::max(framebuffer_height, 1);
    metrics.scale_x = float(metrics.framebuffer_width) / float(metrics.logical_width);
    metrics.scale_y = float(metrics.framebuffer_height) / float(metrics.logical_height);
    return rmlui_bgfx::sanitize_surface_metrics(metrics);
}

[[nodiscard]] bool query_native_window(SDL_Window* window, bgfx::PlatformData& platform_data)
{
#if defined(SDL_PLATFORM_LINUX)
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    platform_data.ndt = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
    const std::uint64_t x11_window =
        SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    if (platform_data.ndt && x11_window != 0) {
        platform_data.nwh = reinterpret_cast<void*>(static_cast<std::uintptr_t>(x11_window));
        return true;
    }
    Rml::Log::Message(Rml::Log::LT_ERROR,
                      "SDL X11 native window handles unavailable. Try SDL_VIDEODRIVER=x11.");
    return false;
#elif defined(SDL_PLATFORM_ANDROID)
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    platform_data.nwh =
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr);
    return platform_data.nwh != nullptr;
#else
    platform_data.nwh = window;
    return true;
#endif
}

struct BackendData {
    explicit BackendData(SDL_Window* in_window) : system_interface(in_window), window(in_window) {}

    SystemInterface_SDL system_interface;
    TextInputMethodEditor_SDL text_input_method_editor;
    SampleShaderProvider shaders;
    SampleTextureLoader textures;
    SampleDiagnostics diagnostics;
    std::unique_ptr<rmlui_bgfx::PrecompiledMaterialShaderProvider> material_shaders;
    std::unique_ptr<rmlui_bgfx::RenderInterface> render_interface;
    SDL_Window* window = nullptr;
    bool running = true;
    bool bgfx_initialized = false;
    Rml::Vector2f mouse_position = Rml::Vector2f(-1.0f, -1.0f);
    bool mouse_position_valid = false;
};

static Rml::UniquePtr<BackendData> data;

void resize_backend(SDL_Window* window)
{
    if (!data || !data->render_interface)
        return;
    const rmlui_bgfx::SurfaceMetrics surface = query_surface(window);
    bgfx::reset(static_cast<std::uint32_t>(surface.framebuffer_width),
                static_cast<std::uint32_t>(surface.framebuffer_height), BGFX_RESET_VSYNC);
    data->render_interface->resize(surface);
}

} // namespace

bool Backend::Initialize(const char* window_name, int width, int height, bool allow_resize)
{
    RMLUI_ASSERT(!data);

#if defined(SDL_PLATFORM_LINUX)
    if (!SDL_getenv("SDL_VIDEODRIVER"))
        SDL_setenv_unsafe("SDL_VIDEODRIVER", "x11", 0);
#endif

    SDL_SetHint(SDL_HINT_IME_IMPLEMENTED_UI, "composition");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        Rml::Log::Message(Rml::Log::LT_ERROR, "SDL_Init failed: %s", SDL_GetError());
        return false;
    }

    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");

    const float window_size_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, window_name);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER,
                          int(width * window_size_scale));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER,
                          int(height * window_size_scale));
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, allow_resize);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, true);

    SDL_Window* window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (!window) {
        Rml::Log::Message(Rml::Log::LT_ERROR, "SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return false;
    }

    bgfx::PlatformData platform_data{};
    if (!query_native_window(window, platform_data)) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return false;
    }

    const rmlui_bgfx::SurfaceMetrics surface = query_surface(window);

    static SampleBgfxCallback bgfx_callback(env_flag_enabled("RMLUI_BGFX_BGFX_DEBUG"));

    bgfx::Init init;
    init.type = bgfx::RendererType::OpenGL;
    init.callback = &bgfx_callback;
    init.platformData = platform_data;
    init.resolution.width = static_cast<std::uint32_t>(surface.framebuffer_width);
    init.resolution.height = static_cast<std::uint32_t>(surface.framebuffer_height);
    init.resolution.reset = BGFX_RESET_VSYNC;
    init.debug = env_flag_enabled("RMLUI_BGFX_BGFX_DEBUG");

    if (!bgfx::init(init)) {
        Rml::Log::Message(Rml::Log::LT_ERROR, "bgfx::init failed");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return false;
    }

    bgfx::setDebug(env_flag_enabled("RMLUI_BGFX_BGFX_DEBUG") ? BGFX_DEBUG_TEXT : BGFX_DEBUG_NONE);

    data = Rml::MakeUnique<BackendData>(window);
    data->bgfx_initialized = true;

    rmlui_bgfx::PrecompiledMaterialShaderProviderConfig material_config;
    material_config.root_directory = RMLUI_BGFX_SAMPLE_SHADER_DIR;
    material_config.diagnostics = &data->diagnostics;
    data->material_shaders =
        std::make_unique<rmlui_bgfx::PrecompiledMaterialShaderProvider>(std::move(material_config));
    data->material_shaders->register_shader(std::string("abi_time"),
                                            std::string("material_abi_time"));
    data->material_shaders->register_shader(std::string("abi_dimensions"),
                                            std::string("material_abi_dimensions"));
    data->material_shaders->register_shader(std::string("abi_dpi"),
                                            std::string("material_abi_dpi"));
    data->material_shaders->register_shader(std::string("abi_mouse"),
                                            std::string("material_abi_mouse"));
    data->material_shaders->register_shader(std::string("abi_combined"),
                                            std::string("material_abi_combined"));

    rmlui_bgfx::RendererConfig config;
    config.surface = surface;
    config.views = rmlui_bgfx::ViewRange{0, 255};
    config.shaders = &data->shaders;
    config.textures = &data->textures;
    config.diagnostics = &data->diagnostics;
    config.material_shaders = data->material_shaders.get();
    config.render_path = render_path_from_env();
    config.blur_sample_bounds_mode = blur_sample_bounds_mode_from_env();
    config.reference_msaa_samples = reference_msaa_samples_from_env();
    config.trace_filter_pipeline = trace_filter_pipeline_from_env();
    config.bounded_transform_layers = bounded_transform_layers_from_env();

    data->render_interface = std::make_unique<rmlui_bgfx::RenderInterface>(config);
    if (!*data->render_interface) {
        Rml::Log::Message(Rml::Log::LT_ERROR, "Failed to initialize rmlui-bgfx render interface");
        data.reset();
        bgfx::shutdown();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return false;
    }

    Rml::SetTextInputHandler(&data->text_input_method_editor);
    return true;
}

void Backend::Shutdown()
{
    RMLUI_ASSERT(data);

    SDL_Window* window = data->window;
    const bool bgfx_initialized = data->bgfx_initialized;

    Rml::SetTextInputHandler(nullptr);
    data->render_interface.reset();
    data.reset();

    if (bgfx_initialized)
        bgfx::shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
}

Rml::SystemInterface* Backend::GetSystemInterface()
{
    RMLUI_ASSERT(data);
    return &data->system_interface;
}

Rml::RenderInterface* Backend::GetRenderInterface()
{
    RMLUI_ASSERT(data && data->render_interface);
    return data->render_interface.get();
}

bool Backend::ProcessEvents(Rml::Context* context, KeyDownCallback key_down_callback,
                            bool power_save)
{
    RMLUI_ASSERT(data && context);

    bool result = data->running;
    data->running = true;

    SDL_Event ev;
    bool has_event =
        power_save
            ? SDL_WaitEventTimeout(
                  &ev, static_cast<int>(Rml::Math::Min(context->GetNextUpdateDelay(), 10.0) * 1000))
            : SDL_PollEvent(&ev);

    while (has_event) {
        bool propagate_event = true;
        switch (ev.type) {
        case SDL_EVENT_QUIT:
            propagate_event = false;
            result = false;
            break;
        case SDL_EVENT_KEY_DOWN: {
            propagate_event = false;
            const Rml::Input::KeyIdentifier key = RmlSDL::ConvertKey(ev.key.key);
            const int key_modifier = RmlSDL::GetKeyModifierState();
            const float native_dp_ratio = SDL_GetWindowDisplayScale(data->window);

            if (key_down_callback &&
                !key_down_callback(context, key, key_modifier, native_dp_ratio, true))
                break;
            if (!RmlSDL::InputEventHandler(context, data->window, ev))
                break;
            if (key_down_callback &&
                !key_down_callback(context, key, key_modifier, native_dp_ratio, false))
                break;
            break;
        }
        case SDL_EVENT_TEXT_EDITING:
            propagate_event = false;
            data->text_input_method_editor.HandleEdit(ev.edit);
            break;
        case SDL_EVENT_MOUSE_MOTION:
            data->mouse_position = Rml::Vector2f(ev.motion.x, ev.motion.y);
            data->mouse_position_valid = true;
            if (data->material_shaders) {
                data->material_shaders->set_mouse_position(data->mouse_position, true);
            }
            break;
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            data->mouse_position = Rml::Vector2f(-1.0f, -1.0f);
            data->mouse_position_valid = false;
            if (data->material_shaders) {
                data->material_shaders->clear_mouse_position();
            }
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            resize_backend(data->window);
            break;
        default:
            break;
        }

        if (propagate_event)
            RmlSDL::InputEventHandler(context, data->window, ev);

        has_event = SDL_PollEvent(&ev);
    }

    return result;
}

void Backend::RequestExit()
{
    RMLUI_ASSERT(data);
    data->running = false;
}

void Backend::BeginFrame()
{
    RMLUI_ASSERT(data && data->render_interface);
    data->render_interface->begin_frame();
}

void Backend::PresentFrame()
{
    RMLUI_ASSERT(data && data->render_interface);
    data->render_interface->end_frame();
    bgfx::frame();
    RMLUI_FrameMark;
}
