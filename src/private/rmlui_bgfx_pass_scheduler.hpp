#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace rmlui_bgfx {

using RmlUiViewId = uint16_t;

enum class RmlUiPassKind {
    Geometry,
    Clear,
    Resolve,
    Copy,
    Postprocess,
    LayerComposite,
    FinalComposite,
};

enum class RmlUiPassReason {
    OrdinaryGeometry,
    Gradient,
    ClipMask,
    StencilNormalize,
    BaseClear,
    LayerClear,
    StencilClear,
    FilterCopy,
    FilterOpacity,
    FilterColorMatrix,
    FilterMaskImage,
    FilterBlur,
    FilterDropShadow,
    FilterDropShadowComposite,
    LayerScratchCopy,
    LayerComposite,
    FinalComposite,
    SaveTextureCopy,
    SaveMaskCopy,
    OtherCopy,
    Other,
};

struct RmlUiPassRequest {
    RmlUiPassKind kind = RmlUiPassKind::Geometry;
    uintptr_t framebuffer = 0;
    uint16_t bgfx_framebuffer_idx = std::numeric_limits<uint16_t>::max();
    bool clears_color = false;
    bool clears_stencil = false;
    int x = 0;
    int y = 0;
    int width = 1;
    int height = 1;
    const char* name = "RmlUi";
    RmlUiPassReason reason = RmlUiPassReason::Other;
};

struct RmlUiPass {
    RmlUiViewId view = 0;
    RmlUiPassRequest request;
    bool reused = false;
};

class RmlUiRenderPassScheduler {
public:
    RmlUiRenderPassScheduler(RmlUiViewId begin, RmlUiViewId end);

    void reset();
    [[nodiscard]] std::optional<RmlUiPass> acquire(const RmlUiPassRequest& request);
    [[nodiscard]] bool exhausted() const { return m_exhausted; }
    [[nodiscard]] const char* error() const { return m_error.c_str(); }
    [[nodiscard]] const std::vector<RmlUiPass>& passes() const { return m_passes; }

private:
    [[nodiscard]] bool can_reuse_current_pass(const RmlUiPassRequest& request) const;

    RmlUiViewId m_begin = 0;
    RmlUiViewId m_end = 0;
    RmlUiViewId m_next = 0;
    bool m_exhausted = false;
    std::string m_error;
    std::optional<RmlUiPass> m_current;
    std::vector<RmlUiPass> m_passes;
};

} // namespace rmlui_bgfx
