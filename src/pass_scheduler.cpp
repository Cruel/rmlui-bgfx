#include "rmlui_bgfx_pass_scheduler.hpp"

#include <cstdio>

namespace rmlui_bgfx {

namespace {

[[nodiscard]] bool contains_rect(const RmlUiPassRequest& outer, const RmlUiPassRequest& inner)
{
    return outer.x <= inner.x && outer.y <= inner.y &&
           outer.x + outer.width >= inner.x + inner.width &&
           outer.y + outer.height >= inner.y + inner.height;
}

[[nodiscard]] bool is_geometry_like_reason(RmlUiPassReason reason)
{
    switch (reason) {
    case RmlUiPassReason::OrdinaryGeometry:
    case RmlUiPassReason::Gradient:
    case RmlUiPassReason::ClipMask:
        return true;
    case RmlUiPassReason::StencilNormalize:
    case RmlUiPassReason::BaseClear:
    case RmlUiPassReason::LayerClear:
    case RmlUiPassReason::StencilClear:
    case RmlUiPassReason::FilterCopy:
    case RmlUiPassReason::FilterOpacity:
    case RmlUiPassReason::FilterColorMatrix:
    case RmlUiPassReason::FilterMaskImage:
    case RmlUiPassReason::FilterBlur:
    case RmlUiPassReason::FilterDropShadow:
    case RmlUiPassReason::FilterDropShadowComposite:
    case RmlUiPassReason::LayerScratchCopy:
    case RmlUiPassReason::LayerComposite:
    case RmlUiPassReason::FinalComposite:
    case RmlUiPassReason::SaveTextureCopy:
    case RmlUiPassReason::SaveMaskCopy:
    case RmlUiPassReason::OtherCopy:
    case RmlUiPassReason::Other:
        return false;
    }
    return false;
}

} // namespace

RmlUiRenderPassScheduler::RmlUiRenderPassScheduler(RmlUiViewId begin, RmlUiViewId end)
    : m_begin(begin), m_end(end), m_next(begin)
{
}

void RmlUiRenderPassScheduler::reset()
{
    m_next = m_begin;
    m_exhausted = false;
    m_error.clear();
    m_current.reset();
    m_passes.clear();
}

bool RmlUiRenderPassScheduler::can_reuse_current_pass(const RmlUiPassRequest& request) const
{
    // bgfx view reuse is constrained by GL3 ordering semantics. Clears are barriers, and only
    // geometry-like draws may merge into a preceding compatible clear view; composites/copies and
    // postprocess passes stay separate unless their exact non-clear framebuffer/viewport matches.
    if (!m_current || request.clears_color || request.clears_stencil) {
        return false;
    }
    const RmlUiPassRequest& current = m_current->request;
    if (current.framebuffer != request.framebuffer) {
        return false;
    }
    if (!current.clears_color && !current.clears_stencil) {
        return current.x == request.x && current.y == request.y && current.width == request.width &&
               current.height == request.height;
    }
    return current.kind == RmlUiPassKind::Clear && is_geometry_like_reason(request.reason) &&
           contains_rect(current, request);
}

std::optional<RmlUiPass> RmlUiRenderPassScheduler::acquire(const RmlUiPassRequest& request)
{
    if (can_reuse_current_pass(request)) {
        RmlUiPass reused = *m_current;
        reused.reused = true;
        return reused;
    }

    if (m_next > m_end) {
        if (!m_exhausted) {
            m_error = "RmlUi bgfx view range exhausted";
            std::fprintf(stderr, "[rmlui] %s (%u..%u)\n", m_error.c_str(), unsigned(m_begin),
                         unsigned(m_end));
        }
        m_exhausted = true;
        return std::nullopt;
    }

    RmlUiPass pass{m_next++, request, false};
    m_passes.push_back(pass);
    m_current = pass;
    return pass;
}

} // namespace rmlui_bgfx
