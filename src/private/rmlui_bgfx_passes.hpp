#pragma once

#include "rmlui_bgfx_pass_scheduler.hpp"
#include "rmlui_bgfx_types.hpp"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <optional>

namespace rmlui_bgfx {

class BgfxPassBuilder {
public:
    BgfxPassBuilder(RmlUiViewId begin, RmlUiViewId end, PerfCounters* perf = nullptr);

    void set_perf_counters(PerfCounters* perf);
    void begin_frame(int framebuffer_width, int framebuffer_height);

    [[nodiscard]] std::optional<RmlUiPass>
    geometry(bgfx::FrameBufferHandle target, int width, int height,
             const char* name = "RmlUi.Geometry",
             RmlUiPassReason reason = RmlUiPassReason::OrdinaryGeometry);
    [[nodiscard]] std::optional<RmlUiPass> base_clear(bgfx::FrameBufferHandle target, int width,
                                                      int height);
    [[nodiscard]] std::optional<RmlUiPass> layer_clear(bgfx::FrameBufferHandle target, int width,
                                                       int height);
    [[nodiscard]] std::optional<RmlUiPass>
    stencil_clear(bgfx::FrameBufferHandle target, LocalFbRect local_rect, uint8_t stencil_value);
    [[nodiscard]] std::optional<RmlUiPass>
    composite(bgfx::FrameBufferHandle target, LocalFbRect destination_rect, RmlUiPassKind kind,
              const char* name, RmlUiPassReason reason = RmlUiPassReason::LayerComposite);
    [[nodiscard]] std::optional<RmlUiPass>
    copy(bgfx::FrameBufferHandle target, int width, int height, const char* name,
         RmlUiPassReason reason = RmlUiPassReason::OtherCopy);
    [[nodiscard]] std::optional<RmlUiPass>
    postprocess(bgfx::FrameBufferHandle target, int width, int height, const char* name,
                RmlUiPassReason reason = RmlUiPassReason::Other);

    [[nodiscard]] bool exhausted() const;
    [[nodiscard]] const char* error() const;

private:
    [[nodiscard]] std::optional<RmlUiPass> acquire(RmlUiPassRequest request,
                                                   bgfx::FrameBufferHandle framebuffer);
    void configure_pass(const RmlUiPass& pass) const;
    void configure_clear(const RmlUiPass& pass, uint16_t clear_flags, uint32_t rgba, float depth,
                         uint8_t stencil) const;

    RmlUiRenderPassScheduler m_scheduler;
    PerfCounters* m_perf = nullptr;
    int m_framebuffer_width = 1;
    int m_framebuffer_height = 1;
};

} // namespace rmlui_bgfx
