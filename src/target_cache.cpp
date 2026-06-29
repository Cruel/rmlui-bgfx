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

const char* texture_format_name(bgfx::TextureFormat::Enum format)
{
    switch (format) {
    case bgfx::TextureFormat::RGBA8:
        return "RGBA8";
    case bgfx::TextureFormat::D24S8:
        return "D24S8";
    case bgfx::TextureFormat::D0S8:
        return "D0S8";
    case bgfx::TextureFormat::Unknown:
        return "Unknown";
    default:
        return "Other";
    }
}

} // namespace

BgfxTargetCache::BgfxTargetCache(PerfCounters* perf) : m_perf(perf) {}

uint64_t BgfxTargetCache::next_target_generation()
{
    ++m_target_generation_counter;
    if (m_target_generation_counter == 0) {
        ++m_target_generation_counter;
    }
    return m_target_generation_counter;
}

TargetDescriptor BgfxTargetCache::make_layer_target_descriptor(
    const RenderBounds& bounds, bgfx::TextureFormat::Enum stencil_format, bool requested_msaa,
    uint8_t requested_msaa_samples) const
{
    return TargetDescriptor{TargetRole::LayerColorDepth,
                            PostprocessTargetKind::Primary,
                            TargetLifetime::Viewport,
                            bounds.framebuffer,
                            bounds.framebuffer.w,
                            bounds.framebuffer.h,
                            bgfx::TextureFormat::RGBA8,
                            stencil_format,
                            uint8_t(requested_msaa ? requested_msaa_samples : 0),
                            true,
                            true,
                            false,
                            0,
                            "RmlUi.LayerTarget",
                            "layer materialization"};
}

TargetDescriptor BgfxTargetCache::make_postprocess_target_descriptor(
    PostprocessTargetKind kind, const FbRect& bounds, const SurfaceMetrics& surface) const
{
    const bool target_is_full_frame =
        is_full_frame_rect(bounds, surface.framebuffer_width, surface.framebuffer_height);
    return TargetDescriptor{TargetRole::Postprocess,
                            kind,
                            target_is_full_frame ? TargetLifetime::Viewport : TargetLifetime::Frame,
                            bounds,
                            bounds.w,
                            bounds.h,
                            bgfx::TextureFormat::RGBA8,
                            bgfx::TextureFormat::Unknown,
                            0,
                            false,
                            true,
                            bgfx::getCaps() &&
                                (bgfx::getCaps()->supported & BGFX_CAPS_TEXTURE_BLIT) != 0,
                            0,
                            "RmlUi.PostprocessTarget",
                            target_is_full_frame ? "full-frame viewport postprocess"
                                                 : "bounded frame postprocess"};
}

void BgfxTargetCache::log_target_allocation_failure(const TargetDescriptor& desc,
                                                    const char* step) const
{
    std::fprintf(stderr,
                 "[rmlui] failed to allocate target step=%s role=%s kind=%s lifetime=%s "
                 "bounds=(%d,%d %dx%d) size=%dx%d color=%s depth=%s msaa=%u label=%s reason=%s\n",
                 step ? step : "unknown", target_role_name(desc.role),
                 desc.role == TargetRole::Postprocess
                     ? postprocess_target_kind_name(desc.postprocess_kind)
                     : "n/a",
                 target_lifetime_name(desc.lifetime), desc.bounds.x, desc.bounds.y, desc.bounds.w,
                 desc.bounds.h, desc.texture_width, desc.texture_height,
                 texture_format_name(desc.color_format), texture_format_name(desc.depth_stencil_format),
                 unsigned(desc.msaa_samples), desc.debug_label ? desc.debug_label : "n/a",
                 desc.reason ? desc.reason : "n/a");
}

BgfxTargetCache::~BgfxTargetCache()
{
    destroy_layers();
    destroy_postprocess_targets();
}

void BgfxTargetCache::set_perf_counters(PerfCounters* perf) { m_perf = perf; }

void BgfxTargetCache::begin_frame()
{
    ++m_frame_generation;
    if (m_frame_generation == 0) {
        ++m_frame_generation;
    }

    // GL3 keeps fixed-role postprocess targets viewport-scoped. The optimized path preserves that
    // for full-frame role targets while keeping bounded targets frame-scoped so scrolling through
    // many slightly different filter bounds cannot grow memory across frames.
    for (auto it = m_postprocess_targets.begin(); it != m_postprocess_targets.end();) {
        if (it->lifetime == TargetLifetime::Frame || !bgfx::isValid(it->framebuffer) ||
            !bgfx::isValid(it->color)) {
            destroy_render_target(*it);
            it = m_postprocess_targets.erase(it);
        } else {
            ++it;
        }
    }
    m_postprocess_pool.reset_resources();
    for (const RenderTargetRecord& target : m_postprocess_targets) {
        if (target.lifetime == TargetLifetime::Viewport && bgfx::isValid(target.framebuffer)) {
            m_postprocess_pool.mark_allocated(target.kind);
        }
    }
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
                                          bgfx::TextureFormat::Enum stencil_format,
                                          uint8_t msaa_samples)
{
    uint64_t msaa_flag = 0;
    switch (msaa_samples) {
    case 2:
        msaa_flag = BGFX_TEXTURE_RT_MSAA_X2;
        break;
    case 4:
        msaa_flag = BGFX_TEXTURE_RT_MSAA_X4;
        break;
    case 8:
        msaa_flag = BGFX_TEXTURE_RT_MSAA_X8;
        break;
    case 16:
        msaa_flag = BGFX_TEXTURE_RT_MSAA_X16;
        break;
    default:
        break;
    }
    const uint64_t msaa_color_flags = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | msaa_flag;
    const uint64_t msaa_depth_flags = BGFX_TEXTURE_RT_WRITE_ONLY | msaa_flag;
    const bool requested_msaa =
        msaa_flag != 0 &&
        bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::RGBA8, msaa_color_flags) &&
        bgfx::isTextureValid(0, false, 1, stencil_format, msaa_depth_flags);
    const TargetDescriptor descriptor =
        make_layer_target_descriptor(bounds, stencil_format, requested_msaa, msaa_samples);
    LayerRecord& layer_record = prepare_virtual_layer_slot(slot);
    if (m_perf) {
        m_perf->update_layer_max(uint32_t(bounds.framebuffer.w), uint32_t(bounds.framebuffer.h));
    }
    if (bgfx::isValid(layer_record.framebuffer) &&
        layer_record.texture_width == descriptor.texture_width &&
        layer_record.texture_height == descriptor.texture_height &&
        layer_record.msaa_enabled == requested_msaa &&
        layer_record.depth_stencil_format == descriptor.depth_stencil_format) {
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

    const uint64_t color_flags =
        requested_msaa ? msaa_color_flags
                       : (BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    const uint64_t depth_flags = requested_msaa ? msaa_depth_flags : BGFX_TEXTURE_RT_WRITE_ONLY;
    if (stencil_format == bgfx::TextureFormat::Unknown) {
        log_target_allocation_failure(descriptor, "stencil format");
        return false;
    }
    bgfx::TextureHandle color =
        bgfx::createTexture2D(uint16_t(descriptor.texture_width),
                              uint16_t(descriptor.texture_height), false, 1,
                              descriptor.color_format, color_flags);
    bgfx::TextureHandle depth =
        bgfx::createTexture2D(uint16_t(descriptor.texture_width),
                              uint16_t(descriptor.texture_height), false, 1,
                              stencil_format, depth_flags);
    if (!bgfx::isValid(color) || !bgfx::isValid(depth)) {
        if (bgfx::isValid(color)) {
            bgfx::destroy(color);
        }
        if (bgfx::isValid(depth)) {
            bgfx::destroy(depth);
        }
        log_target_allocation_failure(descriptor, !bgfx::isValid(color) ? "color texture"
                                                                        : "depth-stencil texture");
        return false;
    }

    std::array<bgfx::TextureHandle, 2> attachments{color, depth};
    bgfx::FrameBufferHandle framebuffer =
        bgfx::createFrameBuffer(uint8_t(attachments.size()), attachments.data(), true);
    if (!bgfx::isValid(framebuffer)) {
        bgfx::destroy(color);
        bgfx::destroy(depth);
        log_target_allocation_failure(descriptor, "framebuffer");
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
    layer_record.texture_width = descriptor.texture_width;
    layer_record.texture_height = descriptor.texture_height;
    layer_record.target_lifetime = descriptor.lifetime;
    layer_record.target_generation = next_target_generation();
    layer_record.color_format = descriptor.color_format;
    layer_record.depth_stencil_format = descriptor.depth_stencil_format;
    layer_record.msaa_samples = descriptor.msaa_samples;
    layer_record.msaa_enabled = requested_msaa;
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
    // The role is part of the target identity. A Primary-sized target and a BlendMask-sized target
    // are not interchangeable even when their physical dimensions match.
    const FbRect clamped_bounds =
        clamp_to_surface(align_outward_for_render_target(bounds), surface);
    if (is_empty(clamped_bounds)) {
        return nullptr;
    }
    const TargetDescriptor descriptor = make_postprocess_target_descriptor(kind, clamped_bounds, surface);
    const int work_w = descriptor.texture_width;
    const int work_h = descriptor.texture_height;
    const bool target_is_full_frame =
        is_full_frame_rect(clamped_bounds, surface.framebuffer_width, surface.framebuffer_height);
    if (m_perf) {
        m_perf->add_postprocess_target_use(uint32_t(work_w), uint32_t(work_h),
                                           target_is_full_frame);
    }
    for (RenderTargetRecord& target : m_postprocess_targets) {
        if (target.kind == kind && bgfx::isValid(target.framebuffer) &&
            target.texture_width == work_w && target.texture_height == work_h &&
            target.color_format == descriptor.color_format && target.lifetime == descriptor.lifetime &&
            target.msaa_samples == descriptor.msaa_samples &&
            (target.lifetime != TargetLifetime::Viewport ||
             (target.surface_width == surface.framebuffer_width &&
              target.surface_height == surface.framebuffer_height))) {
            target.bounds = descriptor.bounds;
            target.last_used_frame = m_frame_generation;
            return &target;
        }
    }

    uint64_t flags = BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
    if (descriptor.blit_destination) {
        flags |= BGFX_TEXTURE_BLIT_DST;
    }
    bgfx::TextureHandle color = bgfx::createTexture2D(uint16_t(work_w), uint16_t(work_h), false, 1,
                                                      descriptor.color_format, flags);
    if (!bgfx::isValid(color)) {
        log_target_allocation_failure(descriptor, "color texture");
        return nullptr;
    }
    bgfx::FrameBufferHandle framebuffer = bgfx::createFrameBuffer(1, &color, true);
    if (!bgfx::isValid(framebuffer)) {
        bgfx::destroy(color);
        log_target_allocation_failure(descriptor, "framebuffer");
        return nullptr;
    }
    m_postprocess_targets.push_back({framebuffer,
                                     color,
                                     descriptor.bounds,
                                     descriptor.texture_width,
                                     descriptor.texture_height,
                                     kind,
                                     descriptor.lifetime,
                                     next_target_generation(),
                                     descriptor.color_format,
                                     descriptor.msaa_samples,
                                     m_frame_generation,
                                     m_frame_generation,
                                     target_is_full_frame,
                                     surface.framebuffer_width,
                                     surface.framebuffer_height});
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
