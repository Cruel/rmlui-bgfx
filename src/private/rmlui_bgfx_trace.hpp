#pragma once

#include <rmlui_bgfx/config.hpp>

#include "rmlui_bgfx_pass_scheduler.hpp"
#include "rmlui_bgfx_types.hpp"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <deque>
#include <sstream>
#include <string>
#include <string_view>

namespace rmlui_bgfx {

[[nodiscard]] const char* trace_category_name(TraceCategory category);
[[nodiscard]] const char* trace_render_path_name(RenderPath path);
[[nodiscard]] const char* trace_pass_kind_name(RmlUiPassKind kind);
[[nodiscard]] const char* trace_pass_reason_name(RmlUiPassReason reason);
[[nodiscard]] uint64_t trace_categories_from_csv(std::string_view value);
[[nodiscard]] bool trace_parse_frame_range(std::string_view value, uint64_t& begin,
                                           uint64_t& end);

class RenderTrace {
public:
    RenderTrace() = default;
    RenderTrace(RenderPath path, TraceOptions options);

    void configure(RenderPath path, TraceOptions options);
    void begin_frame(uint64_t frame_index, SurfaceMetrics surface);

    [[nodiscard]] uint64_t frame_index() const { return m_frame_index; }
    [[nodiscard]] bool enabled(TraceCategory category, const char* operation = nullptr,
                               const char* reason = nullptr, const char* stage = nullptr) const;
    [[nodiscard]] bool category_enabled(TraceCategory category) const;
    [[nodiscard]] bool include_reused_passes() const { return m_options.include_reused_passes; }
    [[nodiscard]] bool include_skips() const { return m_options.include_skips; }

    void emit(TraceCategory category, const char* stage, const char* operation,
              std::string_view fields);
    void dump_on_failure(std::string_view reason);

private:
    [[nodiscard]] bool frame_selected() const;
    [[nodiscard]] bool operation_selected(const char* operation) const;
    [[nodiscard]] bool reason_selected(const char* reason) const;
    [[nodiscard]] std::string prefix(TraceCategory category, const char* stage,
                                     const char* operation) const;
    void remember(std::string line);

    TraceOptions m_options;
    RenderPath m_path = RenderPath::Optimized;
    SurfaceMetrics m_surface{};
    uint64_t m_frame_index = 0;
    std::deque<std::string> m_ring;
};

class RenderTraceLine {
public:
    RenderTraceLine(RenderTrace& trace, TraceCategory category, const char* stage,
                    const char* operation);

    template<typename T> RenderTraceLine& field(const char* name, const T& value)
    {
        m_fields << ' ' << name << '=' << value;
        return *this;
    }

    RenderTraceLine& field(const char* name, bool value);
    RenderTraceLine& field(const char* name, const char* value);
    RenderTraceLine& field(const char* name, std::string_view value);
    RenderTraceLine& handle(const char* name, bgfx::TextureHandle handle);
    RenderTraceLine& handle(const char* name, bgfx::FrameBufferHandle handle);
    RenderTraceLine& fb_rect(const char* name, FbRect rect);
    RenderTraceLine& local_rect(const char* name, LocalFbRect rect);
    RenderTraceLine& logical_rect(const char* name, LogicalRect rect);
    RenderTraceLine& rml_rect(const char* name, const Rml::Rectanglei& rect);
    RenderTraceLine& scissor(const char* name, const ScissorState& scissor);
    RenderTraceLine& texture_region(const char* name, const TextureRegion& region);
    RenderTraceLine& pass_request(const RmlUiPassRequest& request);
    RenderTraceLine& pass(const RmlUiPass& pass);
    RenderTraceLine& reason(const char* value);
    void emit();

private:
    RenderTrace& m_trace;
    TraceCategory m_category;
    const char* m_stage = "unknown";
    const char* m_operation = "unknown";
    std::ostringstream m_fields;
    bool m_emitted = false;
};

#define RMLUI_BGFX_TRACE(trace_ptr, category_value, stage_value, operation_value, body)             \
    do {                                                                                           \
        auto* rmlui_bgfx_trace_ptr__ = (trace_ptr);                                                \
        if (rmlui_bgfx_trace_ptr__ &&                                                              \
            rmlui_bgfx_trace_ptr__->enabled((category_value), (operation_value), nullptr, (stage_value))) {                 \
            ::rmlui_bgfx::RenderTraceLine line(*rmlui_bgfx_trace_ptr__, (category_value),           \
                                               (stage_value), (operation_value));                   \
            body                                                                                   \
            line.emit();                                                                           \
        }                                                                                          \
    } while (false)

#define RMLUI_BGFX_TRACE_FAILURE(trace_ptr, operation_value, reason_value, body)                    \
    do {                                                                                           \
        auto* rmlui_bgfx_trace_ptr__ = (trace_ptr);                                                \
        if (rmlui_bgfx_trace_ptr__ &&                                                              \
            rmlui_bgfx_trace_ptr__->enabled(::rmlui_bgfx::TraceCategory::Failure,                  \
                                            (operation_value), (reason_value))) {                   \
            ::rmlui_bgfx::RenderTraceLine line(*rmlui_bgfx_trace_ptr__,                            \
                                               ::rmlui_bgfx::TraceCategory::Failure, "fail",       \
                                               (operation_value));                                  \
            line.reason(reason_value);                                                             \
            body                                                                                   \
            line.emit();                                                                           \
        }                                                                                          \
    } while (false)

#define RMLUI_BGFX_TRACE_FALLBACK(trace_ptr, operation_value, reason_value, body)                   \
    do {                                                                                           \
        auto* rmlui_bgfx_trace_ptr__ = (trace_ptr);                                                \
        if (rmlui_bgfx_trace_ptr__ &&                                                              \
            rmlui_bgfx_trace_ptr__->enabled(::rmlui_bgfx::TraceCategory::Fallback,                 \
                                            (operation_value), (reason_value))) {                   \
            ::rmlui_bgfx::RenderTraceLine line(*rmlui_bgfx_trace_ptr__,                            \
                                               ::rmlui_bgfx::TraceCategory::Fallback, "fallback",  \
                                               (operation_value));                                  \
            line.reason(reason_value);                                                             \
            body                                                                                   \
            line.emit();                                                                           \
        }                                                                                          \
    } while (false)

} // namespace rmlui_bgfx
