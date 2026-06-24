#pragma once

#include "rmlui_bgfx_draw.hpp"
#include "rmlui_bgfx_passes.hpp"
#include "rmlui_bgfx_target_cache.hpp"
#include "rmlui_bgfx_types.hpp"

#include <RmlUi/Core/Types.h>

#include <functional>
#include <unordered_map>
#include <vector>

namespace rmlui_bgfx {

struct BgfxFilterPipelineContext {
    const std::unordered_map<Rml::CompiledFilterHandle, FilterRecord>& filters;
    const std::unordered_map<Rml::TextureHandle, TextureRecord>& textures;
    const SurfaceMetrics& surface;
    BgfxTargetCache& target_cache;
    BgfxPassBuilder& pass_builder;
    BgfxDrawContext& draw_context;
    BgfxDrawResources resources;
    PerfCounters& perf;
    std::function<bool()> ensure_fullscreen_geometry;
    std::function<void(const char*)> fail_frame;
};

class BgfxFilterPipeline {
public:
    [[nodiscard]] std::vector<FilterRecord>
    resolve(const BgfxFilterPipelineContext& ctx,
            Rml::Span<const Rml::CompiledFilterHandle> filter_handles) const;

    [[nodiscard]] FilterExpansion
    expansion_for(const BgfxFilterPipelineContext& ctx,
                  Rml::Span<const Rml::CompiledFilterHandle> filter_handles) const;

    [[nodiscard]] FilterApplyResult
    apply(const BgfxFilterPipelineContext& ctx, TextureRegion source,
          const RenderBounds& source_bounds,
          Rml::Span<const Rml::CompiledFilterHandle> filter_handles) const;

private:
    [[nodiscard]] bool texture_attached_to_framebuffer(const BgfxFilterPipelineContext& ctx,
                                                       bgfx::TextureHandle texture,
                                                       bgfx::FrameBufferHandle framebuffer) const;
    [[nodiscard]] RenderTargetRecord* safe_destination(const BgfxFilterPipelineContext& ctx,
                                                       bgfx::TextureHandle source,
                                                       RenderTargetRecord* current,
                                                       RenderTargetRecord* other) const;
    [[nodiscard]] bool composite(const BgfxFilterPipelineContext& ctx, const CompositeOp& op) const;

    template<typename F>
    [[nodiscard]] bool
    fullscreen_filter_pass(const BgfxFilterPipelineContext& ctx, bgfx::TextureHandle source,
                           const RenderTargetRecord& destination, const char* name, F&& submit_pass,
                           RmlUiPassReason reason) const
    {
        if (!ctx.ensure_fullscreen_geometry || !ctx.ensure_fullscreen_geometry() ||
            !bgfx::isValid(source) || !bgfx::isValid(destination.framebuffer)) {
            return false;
        }
        if (texture_attached_to_framebuffer(ctx, source, destination.framebuffer)) {
            if (ctx.fail_frame) {
                ctx.fail_frame("fullscreen_filter_pass feedback loop");
            }
            return false;
        }
        const int pass_w = destination.texture_width;
        const int pass_h = destination.texture_height;
        const bool is_full = !is_empty(destination.bounds) && destination.bounds.x == 0 &&
                             destination.bounds.y == 0 &&
                             destination.bounds.w >= ctx.surface.framebuffer_width &&
                             destination.bounds.h >= ctx.surface.framebuffer_height;
        auto pass =
            ctx.pass_builder.postprocess(destination.framebuffer, pass_w, pass_h, name, reason);
        if (!pass) {
            return false;
        }
        ctx.perf.add_postprocess(uint64_t(pass_w) * uint64_t(pass_h), is_full);
        return submit_pass(*pass);
    }
};

} // namespace rmlui_bgfx
