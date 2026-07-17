#include "RmlUi_Backend.h"
#include "RmlUi_Backend_SDL_BGFX_Test.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/ElementDocument.h>

#include <SDL3/SDL.h>

#include <bgfx/bgfx.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace {

struct Pixel {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
    std::uint8_t alpha = 0;
};

[[nodiscard]] bool close_channel(std::uint8_t actual, std::uint8_t expected,
                                 std::uint8_t tolerance = 4)
{
    const int delta = int(actual) - int(expected);
    return std::abs(delta) <= tolerance;
}

[[nodiscard]] bool close_pixel(Pixel actual, Pixel expected, std::uint8_t tolerance = 4)
{
    return close_channel(actual.red, expected.red, tolerance) &&
           close_channel(actual.green, expected.green, tolerance) &&
           close_channel(actual.blue, expected.blue, tolerance);
}

[[nodiscard]] Pixel pixel_at(const BackendTest::Screenshot& screenshot, std::uint32_t x,
                             std::uint32_t y)
{
    if (screenshot.width == 0 || screenshot.height == 0 || screenshot.pitch == 0 ||
        screenshot.bgra8.empty()) {
        return {};
    }
    x = std::min(x, screenshot.width - 1);
    y = std::min(y, screenshot.height - 1);
    if (screenshot.y_flip)
        y = screenshot.height - 1 - y;
    const std::size_t offset = std::size_t(y) * screenshot.pitch + std::size_t(x) * 4u;
    if (offset + 3 >= screenshot.bgra8.size())
        return {};
    return {screenshot.bgra8[offset + 2], screenshot.bgra8[offset + 1], screenshot.bgra8[offset],
            screenshot.bgra8[offset + 3]};
}

void print_pixel(const char* label, Pixel pixel)
{
    std::fprintf(stderr, "%s=(%u,%u,%u,%u)\n", label, unsigned(pixel.red), unsigned(pixel.green),
                 unsigned(pixel.blue), unsigned(pixel.alpha));
}

[[nodiscard]] bool configure_environment(std::string_view render_path, bool preserve)
{
    const std::array settings{
        std::pair{"RMLUI_BGFX_RENDER_PATH", render_path.data()},
        std::pair{"RMLUI_BGFX_PRESERVE_BACKBUFFER", preserve ? "1" : "0"},
        std::pair{"RMLUI_BGFX_VIEW_BEGIN", "1"},
        std::pair{"RMLUI_BGFX_REFERENCE_MSAA", "0"},
    };
    for (const auto& [name, value] : settings) {
        if (SDL_setenv_unsafe(name, value, 1) != 0) {
            std::fprintf(stderr, "Failed to set %s: %s\n", name, SDL_GetError());
            return false;
        }
    }
    return true;
}

[[nodiscard]] int run(std::string_view render_path, bool preserve)
{
    if (!configure_environment(render_path, preserve))
        return 1;

    constexpr int initial_width = 128;
    constexpr int initial_height = 128;
    if (!Backend::Initialize("preserve_backbuffer readback", initial_width, initial_height, false))
        return 1;

    Rml::SetSystemInterface(Backend::GetSystemInterface());
    Rml::SetRenderInterface(Backend::GetRenderInterface());
    if (!Rml::Initialise()) {
        Backend::Shutdown();
        return 1;
    }

    const auto surface = BackendTest::GetSurfaceMetrics();
    Rml::Context* context = Rml::CreateContext(
        "preserve-backbuffer", Rml::Vector2i(surface.logical_width, surface.logical_height));
    if (!context) {
        Rml::Shutdown();
        Backend::Shutdown();
        return 1;
    }

    constexpr std::string_view document_source = R"RML(
<rml>
<head>
<style>
body {
    margin: 0px;
    padding: 0px;
    width: 100%;
    height: 100%;
    background-color: rgba(0, 0, 0, 0);
}
#opaque {
    position: absolute;
    left: 50%;
    top: 0px;
    width: 50%;
    height: 100%;
    background-color: #00ff00;
}
</style>
</head>
<body><div id="opaque"></div></body>
</rml>
)RML";
    Rml::ElementDocument* document = context->LoadDocumentFromMemory(Rml::String(document_source));
    if (!document) {
        Rml::Shutdown();
        Backend::Shutdown();
        return 1;
    }
    document->Show();

    constexpr std::uint32_t sentinel_rgba = 0x204060ffu;
    constexpr Pixel sentinel{0x20, 0x40, 0x60, 0xff};
    constexpr Pixel opaque{0x00, 0xff, 0x00, 0xff};

    BackendTest::Screenshot screenshot;
    BackendTest::ResetScreenshot();
    bool screenshot_requested = false;
    for (int frame = 0; frame < 30 && screenshot.bgra8.empty(); ++frame) {
        context->Update();
        Backend::BeginFrame();

        bgfx::setViewRect(0, 0, 0, static_cast<std::uint16_t>(surface.framebuffer_width),
                          static_cast<std::uint16_t>(surface.framebuffer_height));
        bgfx::setViewClear(0, BGFX_CLEAR_COLOR, sentinel_rgba);
        bgfx::touch(0);

        context->Render();
        if (frame >= 2 && !screenshot_requested) {
            bgfx::requestScreenShot(BGFX_INVALID_HANDLE, "preserve-backbuffer-readback");
            screenshot_requested = true;
        }
        Backend::PresentFrame();
        (void)BackendTest::TakeScreenshot(screenshot);
        if (screenshot.bgra8.empty())
            SDL_Delay(5);
    }

    int result = 0;
    if (screenshot.bgra8.empty()) {
        std::fprintf(stderr, "No bgfx screenshot callback was received\n");
        result = 1;
    } else {
        const Pixel transparent_pixel =
            pixel_at(screenshot, screenshot.width / 4u, screenshot.height / 2u);
        const Pixel opaque_pixel =
            pixel_at(screenshot, (screenshot.width * 3u) / 4u, screenshot.height / 2u);

        const bool transparent_matches = close_pixel(transparent_pixel, sentinel);
        const bool transparent_cleared = transparent_pixel.red <= 8 &&
                                         transparent_pixel.green <= 8 &&
                                         transparent_pixel.blue <= 8;
        const bool opaque_matches = close_pixel(opaque_pixel, opaque);

        if ((preserve && !transparent_matches) || (!preserve && !transparent_cleared) ||
            !opaque_matches) {
            std::fprintf(stderr, "preserve_backbuffer=%d render_path=%.*s readback failed\n",
                         preserve ? 1 : 0, int(render_path.size()), render_path.data());
            print_pixel("transparent", transparent_pixel);
            print_pixel("opaque", opaque_pixel);
            result = 1;
        } else {
            std::printf("preserve_backbuffer=%d render_path=%.*s transparent=(%u,%u,%u) "
                        "opaque=(%u,%u,%u)\n",
                        preserve ? 1 : 0, int(render_path.size()), render_path.data(),
                        unsigned(transparent_pixel.red), unsigned(transparent_pixel.green),
                        unsigned(transparent_pixel.blue), unsigned(opaque_pixel.red),
                        unsigned(opaque_pixel.green), unsigned(opaque_pixel.blue));
        }
    }

    Rml::Shutdown();
    Backend::Shutdown();
    return result;
}

} // namespace

int main(int argc, char** argv)
{
    std::string_view render_path = "reference";
    bool preserve = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--render-path" && index + 1 < argc) {
            render_path = argv[++index];
        } else if (argument == "--preserve" && index + 1 < argc) {
            preserve = std::string_view(argv[++index]) == "1";
        } else {
            std::fprintf(stderr, "usage: %s --render-path <reference|optimized> --preserve <0|1>\n",
                         argv[0]);
            return 2;
        }
    }
    if (render_path != "reference" && render_path != "optimized") {
        std::fprintf(stderr, "Unsupported render path: %.*s\n", int(render_path.size()),
                     render_path.data());
        return 2;
    }
    return run(render_path, preserve);
}
