#pragma once

#include "rmlui_bgfx_filters.hpp"

namespace rmlui_bgfx {

[[nodiscard]] FilterApplyResult
apply_filters_optimized(const BgfxFilterPipeline& pipeline, const BgfxFilterPipelineContext& ctx,
                        TextureRegion source, const RenderBounds& source_bounds,
                        Rml::Span<const Rml::CompiledFilterHandle> filter_handles);

} // namespace rmlui_bgfx
