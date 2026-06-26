#pragma once

#include "rmlui_bgfx_layers.hpp"

namespace rmlui_bgfx {

void composite_layers_optimized(BgfxLayerSystem& layer_system, const BgfxLayerCompositeContext& ctx,
                                Rml::LayerHandle source, Rml::LayerHandle destination,
                                Rml::BlendMode blend_mode,
                                Rml::Span<const Rml::CompiledFilterHandle> filters);

} // namespace rmlui_bgfx
