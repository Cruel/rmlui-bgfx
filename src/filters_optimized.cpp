#include "rmlui_bgfx_filter_paths.hpp"

namespace rmlui_bgfx {

FilterApplyResult apply_filters_optimized(const BgfxFilterPipeline& pipeline,
                                          const BgfxFilterPipelineContext& ctx,
                                          TextureRegion source, const RenderBounds& source_bounds,
                                          Rml::Span<const Rml::CompiledFilterHandle> filter_handles)
{
    // The optimized path owns bounded/filter-target behavior. It currently delegates to
    // the shared implementation so path selection is explicit before we specialize the
    // GL3-compatible blur pipeline further.
    return pipeline.apply_common(ctx, source, source_bounds, filter_handles);
}

} // namespace rmlui_bgfx
