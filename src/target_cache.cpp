#include "rmlui_bgfx_target_cache.hpp"

#include <bx/math.h>

#include <array>
#include <cstdio>

namespace rmlui_bgfx {

namespace {

bool is_full_frame_rect(FbRect rect, int width, int height)
{
    return !is_empty(rect) && rect.x == 0 && rect.y == 0 && rect.w >= width && rect.h >= height;
}

} // namespace

BgfxTargetCache::BgfxTargetCache(PerfCounters* perf) : m_perf(perf) {}

BgfxTargetCache::~BgfxTargetCache()
{
    destroy_layers();
    destroy_postprocess_targets();
}

void BgfxTargetCache::set_perf_counters(PerfCounters* perf) { m_perf = perf; }

void BgfxTargetCache::begin_frame()
{
    // Postprocess targets are cheap scratch resources compared to the correctness hazards of
    // retaining an unbounded set of differently sized bounded targets while scrolling through
    // effect-heavy documents. Keep layer targets cached by slot, but reset postprocess scratch
    // targets each frame until this is replaced by a bounded LRU/per-frame pool.
    destroy_postprocess_targets();
}

LayerRecord& BgfxTargetCache::prepare_virtual_layer_slot(uint32_t slot)
{
    if (slot >= m_layers.size()) {
        m_layers.resize(size_t(slot) + 1);
    }
    return m_layers[size_t(slot)];
}

LayerRecord* BgfxTargetCache::layer(uint32_t slot)
{
    if (slot == LayerPoolPlan::InvalidLayer || size_t(slot) >= m_layers.size()) {
        return nullptr;
    }
    return &m_layers[size_t(slot)];
}

const LayerRecord* BgfxTargetCache::layer(uint32_t slot) const
{
    if (slot == LayerPoolPlan::InvalidLayer || size_t(slot) >= m_layers.size()) {
        return nullptr;
    }
    return &m_layers[size_t(slot)];
}

void BgfxTargetCache::destroy_layer(LayerRecord& layer)
{
    if (bgfx::isValid(layer.framebuffer)) {
        bgfx::destroy(layer.framebuffer);
        if (m_perf) {
            m_perf->add_layer_destroy();
        }
    }
    layer = {};
}

void BgfxTargetCache::destroy_render_target(RenderTargetRecord& target)
{
    if (bgfx::isValid(target.framebuffer)) {
        bgfx::destroy(target.framebuffer);
        if (m_perf) {
            m_perf->add_pp_destroy();
        }
    }
    target = {};
}

void BgfxTargetCache::destroy_layers()
{
    for (LayerRecord& layer_record : m_layers) {
        destroy_layer(layer_record);
    }
    m_layers.clear();
    m_layer_pool.reset_resources();
}

void BgfxTargetCache::destroy_postprocess_targets()
{
    for (RenderTargetRecord& target : m_postprocess_targets) {
        destroy_render_target(target);
    }
    m_postprocess_targets.clear();
    m_postprocess_pool.reset_resources();
}

void BgfxTargetCache::resize(const SurfaceMetrics&)
{
    destroy_layers();
    destroy_postprocess_targets();
}

bool BgfxTargetCache::ensure_layer_target(uint32_t slot, const RenderBounds& bounds,
                                          bgfx::TextureFormat::Enum stencil_format)
{
    LayerRecord& layer_record = prepare_virtual_layer_slot(slot);
    if (m_perf) {
        m_perf->update_layer_max(uint32_t(bounds.framebuffer.w), uint32_t(bounds.framebuffer.h));
    }
    if (bgfx::isValid(layer_record.framebuffer) &&
        layer_record.texture_width == bounds.framebuffer.w &&
        layer_record.texture_height == bounds.framebuffer.h) {
        layer_record.bounds = bounds;
        layer_record.materialized = true;
        bx::mtxOrtho(layer_record.projection, bounds.logical.x, bounds.logical.x + bounds.logical.w,
                     bounds.logical.y + bounds.logical.h, bounds.logical.y, -10000.0f, 10000.0f,
                     0.0f, bgfx::getCaps()->homogeneousDepth);
        return true;
    }

    const LayerKind saved_kind = layer_record.kind;
    const Rml::LayerHandle saved_parent_layer = layer_record.parent_layer;
    const ScissorState saved_push_scissor = layer_record.push_scissor;
    const bool saved_push_transform_valid = layer_record.push_transform_valid;
    const bool saved_recording = layer_record.recording;
    const bool saved_clear_pending = layer_record.clear_pending;
    const GlobalFbRect saved_valid_content_bounds = layer_record.valid_content_bounds;
    const bool saved_has_valid_content_bounds = layer_record.has_valid_content_bounds;
    const ConservativeMaskBounds saved_conservative_mask_bounds =
        layer_record.conservative_mask_bounds;
    const bool saved_content_bounds_transform_fallback =
        layer_record.content_bounds_transform_fallback;
    const bool saved_content_bounds_inverse_mask_fallback =
        layer_record.content_bounds_inverse_mask_fallback;
    const bool saved_clip_mask_enabled = layer_record.clip_mask_enabled;
    const uint8_t saved_stencil_ref = layer_record.stencil_ref;
    const size_t saved_inherited_clip_command_count = layer_record.inherited_clip_command_count;
    std::vector<size_t> saved_clip_commands = std::move(layer_record.clip_commands);
    std::vector<RecordedDrawCommand> saved_commands = std::move(layer_record.commands);
    destroy_layer(layer_record);

    constexpr uint64_t color_flags = BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
    constexpr uint64_t depth_flags = BGFX_TEXTURE_RT_WRITE_ONLY;
    if (stencil_format == bgfx::TextureFormat::Unknown) {
        std::fprintf(stderr, "[rmlui] advanced renderer requires a stencil-capable render "
                             "target; D24S8 is unavailable\n");
        return false;
    }
    bgfx::TextureHandle color =
        bgfx::createTexture2D(uint16_t(bounds.framebuffer.w), uint16_t(bounds.framebuffer.h), false,
                              1, bgfx::TextureFormat::RGBA8, color_flags);
    bgfx::TextureHandle depth =
        bgfx::createTexture2D(uint16_t(bounds.framebuffer.w), uint16_t(bounds.framebuffer.h), false,
                              1, stencil_format, depth_flags);
    if (!bgfx::isValid(color) || !bgfx::isValid(depth)) {
        if (bgfx::isValid(color)) {
            bgfx::destroy(color);
        }
        if (bgfx::isValid(depth)) {
            bgfx::destroy(depth);
        }
        std::fprintf(stderr, "[rmlui] failed to create layer framebuffer attachments\n");
        return false;
    }

    std::array<bgfx::TextureHandle, 2> attachments{color, depth};
    bgfx::FrameBufferHandle framebuffer =
        bgfx::createFrameBuffer(uint8_t(attachments.size()), attachments.data(), true);
    if (!bgfx::isValid(framebuffer)) {
        bgfx::destroy(color);
        bgfx::destroy(depth);
        std::fprintf(stderr, "[rmlui] failed to create layer framebuffer\n");
        return false;
    }

    layer_record.framebuffer = framebuffer;
    layer_record.color = color;
    layer_record.depth_stencil = depth;
    layer_record.bounds = bounds;
    layer_record.valid_content_bounds = saved_valid_content_bounds;
    layer_record.has_valid_content_bounds = saved_has_valid_content_bounds;
    layer_record.conservative_mask_bounds = saved_conservative_mask_bounds;
    layer_record.content_bounds_transform_fallback = saved_content_bounds_transform_fallback;
    layer_record.content_bounds_inverse_mask_fallback = saved_content_bounds_inverse_mask_fallback;
    layer_record.texture_width = bounds.framebuffer.w;
    layer_record.texture_height = bounds.framebuffer.h;
    layer_record.clip_mask_enabled = saved_clip_mask_enabled;
    layer_record.stencil_ref = saved_stencil_ref;
    layer_record.clip_commands = std::move(saved_clip_commands);
    layer_record.inherited_clip_command_count = saved_inherited_clip_command_count;
    layer_record.kind = saved_kind;
    layer_record.parent_layer = saved_parent_layer;
    layer_record.push_scissor = saved_push_scissor;
    layer_record.push_transform_valid = saved_push_transform_valid;
    layer_record.recording = saved_recording;
    layer_record.materialized = true;
    layer_record.clear_pending = saved_clear_pending;
    layer_record.commands = std::move(saved_commands);
    bx::mtxOrtho(layer_record.projection, bounds.logical.x, bounds.logical.x + bounds.logical.w,
                 bounds.logical.y + bounds.logical.h, bounds.logical.y, -10000.0f, 10000.0f, 0.0f,
                 bgfx::getCaps()->homogeneousDepth);
    m_layer_pool.note_allocated(slot);
    if (m_perf) {
        m_perf->add_layer_alloc(uint32_t(bounds.framebuffer.w), uint32_t(bounds.framebuffer.h));
    }
    return true;
}

RenderTargetRecord* BgfxTargetCache::acquire_postprocess_target(PostprocessTargetKind kind,
                                                                FbRect bounds,
                                                                const SurfaceMetrics& surface)
{
    const FbRect clamped_bounds =
        clamp_to_surface(align_outward_for_render_target(bounds), surface);
    if (is_empty(clamped_bounds)) {
        return nullptr;
    }
    const int work_w = clamped_bounds.w;
    const int work_h = clamped_bounds.h;
    const bool target_is_full_frame =
        is_full_frame_rect(clamped_bounds, surface.framebuffer_width, surface.framebuffer_height);
    if (m_perf) {
        m_perf->add_postprocess_target_use(uint32_t(work_w), uint32_t(work_h),
                                           target_is_full_frame);
    }
    for (RenderTargetRecord& target : m_postprocess_targets) {
        if (target.kind == kind && bgfx::isValid(target.framebuffer) &&
            target.texture_width == work_w && target.texture_height == work_h) {
            target.bounds = clamped_bounds;
            return &target;
        }
    }

    uint64_t flags = BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
    if (bgfx::getCaps() && (bgfx::getCaps()->supported & BGFX_CAPS_TEXTURE_BLIT) != 0) {
        flags |= BGFX_TEXTURE_BLIT_DST;
    }
    bgfx::TextureHandle color = bgfx::createTexture2D(uint16_t(work_w), uint16_t(work_h), false, 1,
                                                      bgfx::TextureFormat::RGBA8, flags);
    if (!bgfx::isValid(color)) {
        std::fprintf(stderr, "[rmlui] failed to create postprocess target texture\n");
        return nullptr;
    }
    bgfx::FrameBufferHandle framebuffer = bgfx::createFrameBuffer(1, &color, true);
    if (!bgfx::isValid(framebuffer)) {
        bgfx::destroy(color);
        std::fprintf(stderr, "[rmlui] failed to create postprocess framebuffer\n");
        return nullptr;
    }
    m_postprocess_targets.push_back({framebuffer, color, clamped_bounds, work_w, work_h, kind});
    RenderTargetRecord& target = m_postprocess_targets.back();
    m_postprocess_pool.mark_allocated(kind);
    if (m_perf) {
        m_perf->add_pp_alloc(uint32_t(work_w), uint32_t(work_h));
        if (target_is_full_frame) {
            m_perf->add_full_frame_pp_target();
        } else {
            m_perf->add_bounded_pp_target();
        }
    }
    return &target;
}

} // namespace rmlui_bgfx
