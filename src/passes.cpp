#include "rmlui_bgfx_passes.hpp"

#include "rmlui_bgfx_trace.hpp"

#include <algorithm>
#include <limits>

namespace rmlui_bgfx {

namespace {

[[nodiscard]] uintptr_t framebuffer_key(bgfx::FrameBufferHandle framebuffer)
{
    return bgfx::isValid(framebuffer) ? uintptr_t(framebuffer.idx) + 1u : 0u;
}

[[nodiscard]] bgfx::FrameBufferHandle framebuffer_from_request(const RmlUiPassRequest& request)
{
    if (request.bgfx_framebuffer_idx == std::numeric_limits<uint16_t>::max()) {
        return BGFX_INVALID_HANDLE;
    }
    return bgfx::FrameBufferHandle{request.bgfx_framebuffer_idx};
}

[[nodiscard]] RmlUiPassRequest make_pass_request(RmlUiPassKind kind, int x, int y,
                                                 bool clears_color, bool clears_stencil, int width,
                                                 int height, const char* name,
                                                 RmlUiPassReason reason)
{
    return RmlUiPassRequest{kind,
                            0,
                            std::numeric_limits<uint16_t>::max(),
                            clears_color,
                            clears_stencil,
                            x,
                            y,
                            width,
                            height,
                            name,
                            reason};
}

} // namespace

BgfxPassBuilder::BgfxPassBuilder(RmlUiViewId begin, RmlUiViewId end, PerfCounters* perf)
    : m_scheduler(begin, end), m_perf(perf)
{
}

void BgfxPassBuilder::set_perf_counters(PerfCounters* perf) { m_perf = perf; }

void BgfxPassBuilder::set_trace(RenderTrace* trace) { m_trace = trace; }

void BgfxPassBuilder::begin_frame(int framebuffer_width, int framebuffer_height, int viewport_x,
                                  int viewport_y)
{
    m_framebuffer_width = std::max(framebuffer_width, 1);
    m_framebuffer_height = std::max(framebuffer_height, 1);
    m_viewport_x = std::max(viewport_x, 0);
    m_viewport_y = std::max(viewport_y, 0);
    m_scheduler.reset();
}

std::optional<RmlUiPass> BgfxPassBuilder::geometry(bgfx::FrameBufferHandle target, int width,
                                                   int height, const char* name,
                                                   RmlUiPassReason reason)
{
    return acquire(
        make_pass_request(RmlUiPassKind::Geometry, 0, 0, false, false, width, height, name, reason),
        target);
}

std::optional<RmlUiPass> BgfxPassBuilder::base_clear(bgfx::FrameBufferHandle target, int width,
                                                     int height)
{
    auto pass = acquire(make_pass_request(RmlUiPassKind::Clear, 0, 0, true, true, width, height,
                                          "RmlUi.BaseClear", RmlUiPassReason::BaseClear),
                        target);
    if (pass) {
        configure_clear(*pass, BGFX_CLEAR_COLOR | BGFX_CLEAR_STENCIL, 0x00000000u, 1.0f, 0);
    }
    return pass;
}

std::optional<RmlUiPass> BgfxPassBuilder::layer_clear(bgfx::FrameBufferHandle target, int width,
                                                      int height)
{
    auto pass = acquire(make_pass_request(RmlUiPassKind::Clear, 0, 0, true, true, width, height,
                                          "RmlUi.LayerClear", RmlUiPassReason::LayerClear),
                        target);
    if (pass) {
        configure_clear(*pass, BGFX_CLEAR_COLOR | BGFX_CLEAR_STENCIL, 0x00000000u, 1.0f, 0);
    }
    return pass;
}

std::optional<RmlUiPass> BgfxPassBuilder::stencil_clear(bgfx::FrameBufferHandle target,
                                                        LocalFbRect local_rect,
                                                        uint8_t stencil_value)
{
    auto pass = acquire(make_pass_request(RmlUiPassKind::Clear, local_rect.x, local_rect.y, false,
                                          true, local_rect.w, local_rect.h, "RmlUi.StencilClear",
                                          RmlUiPassReason::StencilClear),
                        target);
    if (pass) {
        configure_clear(*pass, BGFX_CLEAR_STENCIL, 0x00000000u, 1.0f, stencil_value);
    }
    return pass;
}

std::optional<RmlUiPass> BgfxPassBuilder::composite(bgfx::FrameBufferHandle target,
                                                    LocalFbRect destination_rect,
                                                    RmlUiPassKind kind, const char* name,
                                                    RmlUiPassReason reason)
{
    return acquire(make_pass_request(kind, destination_rect.x, destination_rect.y, false, false,
                                     destination_rect.w, destination_rect.h, name, reason),
                   target);
}

std::optional<RmlUiPass> BgfxPassBuilder::copy(bgfx::FrameBufferHandle target, int width,
                                               int height, const char* name, RmlUiPassReason reason)
{
    return acquire(
        make_pass_request(RmlUiPassKind::Copy, 0, 0, false, false, width, height, name, reason),
        target);
}

std::optional<RmlUiPass> BgfxPassBuilder::postprocess(bgfx::FrameBufferHandle target, int width,
                                                      int height, const char* name,
                                                      RmlUiPassReason reason)
{
    return acquire(make_pass_request(RmlUiPassKind::Postprocess, 0, 0, false, false, width, height,
                                     name, reason),
                   target);
}

bool BgfxPassBuilder::exhausted() const { return m_scheduler.exhausted(); }

const char* BgfxPassBuilder::error() const { return m_scheduler.error(); }

std::optional<RmlUiPass> BgfxPassBuilder::acquire(RmlUiPassRequest request,
                                                  bgfx::FrameBufferHandle framebuffer)
{
    if (request.width <= 0) {
        request.width = m_framebuffer_width;
    }
    if (request.height <= 0) {
        request.height = m_framebuffer_height;
    }
    request.framebuffer = framebuffer_key(framebuffer);
    request.bgfx_framebuffer_idx =
        bgfx::isValid(framebuffer) ? framebuffer.idx : std::numeric_limits<uint16_t>::max();
    auto pass = m_scheduler.acquire(request);
    if (pass) {
        if (!pass->reused || (m_trace && m_trace->include_reused_passes() &&
                              m_trace->category_enabled(TraceCategory::Pass))) {
            RMLUI_BGFX_TRACE(m_trace, TraceCategory::Pass, pass->reused ? "reuse" : "acquire",
                             "AcquirePass", { line.pass(*pass); });
        }
        if (!pass->reused) {
            configure_pass(*pass);
        }
        if (m_perf) {
            m_perf->add_pass(pass->reused, request.reason);
        }
    } else {
        RMLUI_BGFX_TRACE_FAILURE(m_trace, "AcquirePass",
                                 m_scheduler.error() && m_scheduler.error()[0]
                                     ? m_scheduler.error()
                                     : "pass acquisition failed",
                                 { line.pass_request(request); });
    }
    return pass;
}

void BgfxPassBuilder::configure_pass(const RmlUiPass& pass) const
{
    const bgfx::ViewId view = pass.view;
    bgfx::setViewName(view, pass.request.name);
    bgfx::setViewMode(view, bgfx::ViewMode::Sequential);
    const bool backbuffer = pass.request.bgfx_framebuffer_idx == bgfx::kInvalidHandle;
    const int viewport_x = backbuffer ? m_viewport_x : 0;
    const int viewport_y = backbuffer ? m_viewport_y : 0;
    bgfx::setViewRect(view, static_cast<uint16_t>(std::max(pass.request.x + viewport_x, 0)),
                      static_cast<uint16_t>(std::max(pass.request.y + viewport_y, 0)),
                      static_cast<uint16_t>(std::max(pass.request.width, 1)),
                      static_cast<uint16_t>(std::max(pass.request.height, 1)));
    bgfx::setViewFrameBuffer(view, framebuffer_from_request(pass.request));
    bgfx::setViewClear(view, BGFX_CLEAR_NONE);
    RMLUI_BGFX_TRACE(m_trace, TraceCategory::Pass, "submit", "ConfigurePass", { line.pass(pass); });
}

void BgfxPassBuilder::configure_clear(const RmlUiPass& pass, uint16_t clear_flags, uint32_t rgba,
                                      float depth, uint8_t stencil) const
{
    bgfx::setViewClear(pass.view, clear_flags, rgba, depth, stencil);
    RMLUI_BGFX_TRACE(m_trace, TraceCategory::Pass, "clear", "ConfigureClear", {
        line.pass(pass);
        line.field("clear_flags", clear_flags);
        line.field("rgba", rgba);
        line.field("depth", depth);
        line.field("stencil", unsigned(stencil));
    });
}

} // namespace rmlui_bgfx
