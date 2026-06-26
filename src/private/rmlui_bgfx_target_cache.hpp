#pragma once

#include <rmlui_bgfx/config.hpp>
#include "rmlui_bgfx_types.hpp"

#include <bgfx/bgfx.h>

#include <cstddef>
#include <deque>
#include <vector>

namespace rmlui_bgfx {

class BgfxTargetCache {
public:
    explicit BgfxTargetCache(PerfCounters* perf = nullptr);
    ~BgfxTargetCache();

    BgfxTargetCache(const BgfxTargetCache&) = delete;
    BgfxTargetCache& operator=(const BgfxTargetCache&) = delete;

    void set_perf_counters(PerfCounters* perf);
    void begin_frame();

    [[nodiscard]] std::vector<LayerRecord>& layers() { return m_layers; }
    [[nodiscard]] const std::vector<LayerRecord>& layers() const { return m_layers; }
    [[nodiscard]] std::deque<RenderTargetRecord>& postprocess_targets()
    {
        return m_postprocess_targets;
    }
    [[nodiscard]] const std::deque<RenderTargetRecord>& postprocess_targets() const
    {
        return m_postprocess_targets;
    }
    [[nodiscard]] LayerPoolPlan& layer_pool() { return m_layer_pool; }
    [[nodiscard]] const LayerPoolPlan& layer_pool() const { return m_layer_pool; }
    [[nodiscard]] PostprocessPoolPlan& postprocess_pool() { return m_postprocess_pool; }
    [[nodiscard]] const PostprocessPoolPlan& postprocess_pool() const { return m_postprocess_pool; }

    [[nodiscard]] LayerRecord& prepare_virtual_layer_slot(uint32_t slot);
    [[nodiscard]] bool ensure_layer_target(uint32_t slot, const RenderBounds& bounds,
                                           bgfx::TextureFormat::Enum stencil_format,
                                           uint8_t msaa_samples);
    [[nodiscard]] LayerRecord* layer(uint32_t slot);
    [[nodiscard]] const LayerRecord* layer(uint32_t slot) const;

    [[nodiscard]] RenderTargetRecord* acquire_postprocess_target(PostprocessTargetKind kind,
                                                                 FbRect bounds,
                                                                 const SurfaceMetrics& surface);

    void destroy_layer(LayerRecord& layer);
    void destroy_render_target(RenderTargetRecord& target);
    void destroy_layers();
    void destroy_postprocess_targets();
    void resize(const SurfaceMetrics& surface);

private:
    PerfCounters* m_perf = nullptr;
    std::vector<LayerRecord> m_layers;
    std::deque<RenderTargetRecord> m_postprocess_targets;
    LayerPoolPlan m_layer_pool;
    PostprocessPoolPlan m_postprocess_pool;
};

} // namespace rmlui_bgfx
