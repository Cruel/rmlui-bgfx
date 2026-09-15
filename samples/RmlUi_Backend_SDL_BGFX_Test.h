#pragma once

#include <rmlui_bgfx/config.hpp>

#include <bgfx/bgfx.h>

#include <cstdint>
#include <vector>

namespace BackendTest {

struct Screenshot {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t pitch = 0;
    bgfx::TextureFormat::Enum format = bgfx::TextureFormat::Count;
    bool y_flip = false;
    std::vector<std::uint8_t> pixels;
};

void ResetScreenshot();
[[nodiscard]] bool TakeScreenshot(Screenshot& out);
[[nodiscard]] rmlui_bgfx::SurfaceMetrics GetSurfaceMetrics();

} // namespace BackendTest
