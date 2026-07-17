#pragma once

#include <rmlui_bgfx/config.hpp>

#include <cstdint>
#include <vector>

namespace BackendTest {

struct Screenshot {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t pitch = 0;
    bool y_flip = false;
    std::vector<std::uint8_t> bgra8;
};

void ResetScreenshot();
[[nodiscard]] bool TakeScreenshot(Screenshot& out);
[[nodiscard]] rmlui_bgfx::SurfaceMetrics GetSurfaceMetrics();

} // namespace BackendTest
