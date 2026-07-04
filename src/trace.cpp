#include "rmlui_bgfx_trace.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <system_error>
#include <utility>

namespace rmlui_bgfx {

namespace {

[[nodiscard]] bool equals_ignore_case(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        const char ca = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i])));
        const char cb = static_cast<char>(std::tolower(static_cast<unsigned char>(b[i])));
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string_view trim(std::string_view value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return value;
}

[[nodiscard]] bool parse_u64(std::string_view value, uint64_t& out)
{
    value = trim(value);
    if (value.empty()) {
        return false;
    }
    uint64_t parsed = 0;
    const char* begin = value.data();
    const char* end = value.data() + value.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end) {
        return false;
    }
    out = parsed;
    return true;
}

[[nodiscard]] bool contains(std::string_view haystack, std::string_view needle)
{
    return needle.empty() || haystack.find(needle) != std::string_view::npos;
}

[[nodiscard]] uint64_t category_mask(std::initializer_list<TraceCategory> categories)
{
    uint64_t mask = 0;
    for (TraceCategory category : categories) {
        mask |= trace_category_bit(category);
    }
    return mask;
}

[[nodiscard]] uint64_t category_token_mask(std::string_view token)
{
    token = trim(token);
    if (token.empty()) {
        return 0;
    }
    if (equals_ignore_case(token, "all")) {
        return category_mask({TraceCategory::Frame,
                              TraceCategory::Surface,
                              TraceCategory::Pass,
                              TraceCategory::Target,
                              TraceCategory::Layer,
                              TraceCategory::Record,
                              TraceCategory::Replay,
                              TraceCategory::Draw,
                              TraceCategory::Clip,
                              TraceCategory::Stencil,
                              TraceCategory::Filter,
                              TraceCategory::Mask,
                              TraceCategory::Texture,
                              TraceCategory::Copy,
                              TraceCategory::Composite,
                              TraceCategory::Shader,
                              TraceCategory::Fallback,
                              TraceCategory::Failure,
                              TraceCategory::Perf});
    }
    if (equals_ignore_case(token, "effects")) {
        return category_mask({TraceCategory::Layer, TraceCategory::Filter, TraceCategory::Mask,
                              TraceCategory::Texture, TraceCategory::Copy,
                              TraceCategory::Composite, TraceCategory::Failure,
                              TraceCategory::Fallback});
    }
    if (equals_ignore_case(token, "scroll")) {
        return category_mask({TraceCategory::Frame, TraceCategory::Surface, TraceCategory::Layer,
                              TraceCategory::Clip, TraceCategory::Stencil, TraceCategory::Filter,
                              TraceCategory::Composite, TraceCategory::Failure});
    }
    if (equals_ignore_case(token, "targets")) {
        return category_mask({TraceCategory::Pass, TraceCategory::Target, TraceCategory::Copy,
                              TraceCategory::Composite, TraceCategory::Failure});
    }
    if (equals_ignore_case(token, "saved")) {
        return category_mask({TraceCategory::Texture, TraceCategory::Mask, TraceCategory::Filter,
                              TraceCategory::Copy, TraceCategory::Composite,
                              TraceCategory::Failure});
    }

    struct NamedCategory {
        std::string_view name;
        TraceCategory category;
    };
    static constexpr NamedCategory kCategories[] = {
        {"frame", TraceCategory::Frame},       {"surface", TraceCategory::Surface},
        {"pass", TraceCategory::Pass},         {"target", TraceCategory::Target},
        {"layer", TraceCategory::Layer},       {"record", TraceCategory::Record},
        {"replay", TraceCategory::Replay},     {"draw", TraceCategory::Draw},
        {"clip", TraceCategory::Clip},         {"stencil", TraceCategory::Stencil},
        {"filter", TraceCategory::Filter},     {"mask", TraceCategory::Mask},
        {"texture", TraceCategory::Texture},   {"copy", TraceCategory::Copy},
        {"composite", TraceCategory::Composite}, {"shader", TraceCategory::Shader},
        {"fallback", TraceCategory::Fallback}, {"failure", TraceCategory::Failure},
        {"perf", TraceCategory::Perf},
    };
    for (const NamedCategory& named : kCategories) {
        if (equals_ignore_case(token, named.name)) {
            return trace_category_bit(named.category);
        }
    }
    return 0;
}

} // namespace

const char* trace_category_name(TraceCategory category)
{
    switch (category) {
    case TraceCategory::Frame:
        return "frame";
    case TraceCategory::Surface:
        return "surface";
    case TraceCategory::Pass:
        return "pass";
    case TraceCategory::Target:
        return "target";
    case TraceCategory::Layer:
        return "layer";
    case TraceCategory::Record:
        return "record";
    case TraceCategory::Replay:
        return "replay";
    case TraceCategory::Draw:
        return "draw";
    case TraceCategory::Clip:
        return "clip";
    case TraceCategory::Stencil:
        return "stencil";
    case TraceCategory::Filter:
        return "filter";
    case TraceCategory::Mask:
        return "mask";
    case TraceCategory::Texture:
        return "texture";
    case TraceCategory::Copy:
        return "copy";
    case TraceCategory::Composite:
        return "composite";
    case TraceCategory::Shader:
        return "shader";
    case TraceCategory::Fallback:
        return "fallback";
    case TraceCategory::Failure:
        return "failure";
    case TraceCategory::Perf:
        return "perf";
    }
    return "unknown";
}

const char* trace_render_path_name(RenderPath path)
{
    switch (path) {
    case RenderPath::Reference:
        return "reference";
    case RenderPath::Optimized:
        return "optimized";
    }
    return "unknown";
}

const char* trace_pass_kind_name(RmlUiPassKind kind)
{
    switch (kind) {
    case RmlUiPassKind::Geometry:
        return "Geometry";
    case RmlUiPassKind::Clear:
        return "Clear";
    case RmlUiPassKind::Resolve:
        return "Resolve";
    case RmlUiPassKind::Copy:
        return "Copy";
    case RmlUiPassKind::Postprocess:
        return "Postprocess";
    case RmlUiPassKind::LayerComposite:
        return "LayerComposite";
    case RmlUiPassKind::FinalComposite:
        return "FinalComposite";
    }
    return "Unknown";
}

const char* trace_pass_reason_name(RmlUiPassReason reason)
{
    switch (reason) {
    case RmlUiPassReason::OrdinaryGeometry:
        return "OrdinaryGeometry";
    case RmlUiPassReason::Gradient:
        return "Gradient";
    case RmlUiPassReason::ClipMask:
        return "ClipMask";
    case RmlUiPassReason::StencilNormalize:
        return "StencilNormalize";
    case RmlUiPassReason::BaseClear:
        return "BaseClear";
    case RmlUiPassReason::LayerClear:
        return "LayerClear";
    case RmlUiPassReason::StencilClear:
        return "StencilClear";
    case RmlUiPassReason::FilterCopy:
        return "FilterCopy";
    case RmlUiPassReason::FilterOpacity:
        return "FilterOpacity";
    case RmlUiPassReason::FilterColorMatrix:
        return "FilterColorMatrix";
    case RmlUiPassReason::FilterMaskImage:
        return "FilterMaskImage";
    case RmlUiPassReason::FilterBlur:
        return "FilterBlur";
    case RmlUiPassReason::FilterDropShadow:
        return "FilterDropShadow";
    case RmlUiPassReason::FilterDropShadowComposite:
        return "FilterDropShadowComposite";
    case RmlUiPassReason::LayerScratchCopy:
        return "LayerScratchCopy";
    case RmlUiPassReason::LayerComposite:
        return "LayerComposite";
    case RmlUiPassReason::FinalComposite:
        return "FinalComposite";
    case RmlUiPassReason::SaveTextureCopy:
        return "SaveTextureCopy";
    case RmlUiPassReason::SaveMaskCopy:
        return "SaveMaskCopy";
    case RmlUiPassReason::OtherCopy:
        return "OtherCopy";
    case RmlUiPassReason::Other:
        return "Other";
    }
    return "Unknown";
}

uint64_t trace_categories_from_csv(std::string_view value)
{
    uint64_t mask = 0;
    while (!value.empty()) {
        const size_t comma = value.find(',');
        const std::string_view token =
            comma == std::string_view::npos ? value : value.substr(0, comma);
        mask |= category_token_mask(token);
        if (comma == std::string_view::npos) {
            break;
        }
        value.remove_prefix(comma + 1);
    }
    return mask;
}

bool trace_parse_frame_range(std::string_view value, uint64_t& begin, uint64_t& end)
{
    value = trim(value);
    if (value.empty()) {
        return false;
    }
    const size_t dash = value.find('-');
    if (dash == std::string_view::npos) {
        uint64_t single = 0;
        if (!parse_u64(value, single)) {
            return false;
        }
        begin = single;
        end = single;
        return true;
    }
    uint64_t parsed_begin = 0;
    uint64_t parsed_end = 0;
    if (!parse_u64(value.substr(0, dash), parsed_begin) ||
        !parse_u64(value.substr(dash + 1), parsed_end)) {
        return false;
    }
    begin = std::min(parsed_begin, parsed_end);
    end = std::max(parsed_begin, parsed_end);
    return true;
}

RenderTrace::RenderTrace(RenderPath path, TraceOptions options) { configure(path, std::move(options)); }

void RenderTrace::configure(RenderPath path, TraceOptions options)
{
    m_path = path;
    if (options.every_n_frames == 0) {
        options.every_n_frames = 1;
    }
    m_options = std::move(options);
    m_ring.clear();
}

void RenderTrace::begin_frame(uint64_t frame_index, SurfaceMetrics surface)
{
    m_frame_index = frame_index;
    m_surface = sanitize_surface_metrics(surface);
}

bool RenderTrace::category_enabled(TraceCategory category) const
{
    return (m_options.categories & trace_category_bit(category)) != 0;
}

bool RenderTrace::enabled(TraceCategory category, const char* operation, const char* reason,
                          const char* stage) const
{
    if (!frame_selected()) {
        return false;
    }
    if (!operation_selected(operation) || !reason_selected(reason)) {
        return false;
    }
    if (!m_options.include_skips && stage && std::string_view(stage) == "skip") {
        return false;
    }
    if (category_enabled(category)) {
        return true;
    }
    return m_options.ring_line_count > 0 && category == TraceCategory::Failure;
}

void RenderTrace::emit(TraceCategory category, const char* stage, const char* operation,
                       std::string_view fields)
{
    std::string line = prefix(category, stage, operation);
    line.append(fields.data(), fields.size());
    remember(line);
    if (category_enabled(category)) {
        std::fprintf(stderr, "%s\n", line.c_str());
    }
}

void RenderTrace::dump_on_failure(std::string_view reason)
{
    if (!m_options.flush_on_failure || m_ring.empty()) {
        return;
    }
    std::fprintf(stderr,
                 "[rmlui-bgfx][trace-dump][frame=%llu][path=%s] reason=%.*s lines=%zu\n",
                 static_cast<unsigned long long>(m_frame_index), trace_render_path_name(m_path),
                 int(reason.size()), reason.data(), m_ring.size());
    for (const std::string& line : m_ring) {
        std::fprintf(stderr, "[rmlui-bgfx][trace-dump] %s\n", line.c_str());
    }
}

bool RenderTrace::frame_selected() const
{
    if (m_options.first_n_frames > 0 && m_frame_index <= m_options.first_n_frames) {
        return true;
    }
    if (m_frame_index < m_options.frame_begin || m_frame_index > m_options.frame_end) {
        return false;
    }
    return m_options.every_n_frames <= 1 ||
           ((m_frame_index - m_options.frame_begin) % m_options.every_n_frames) == 0;
}

bool RenderTrace::operation_selected(const char* operation) const
{
    return m_options.operation_filter.empty() ||
           contains(operation ? std::string_view(operation) : std::string_view{},
                    m_options.operation_filter);
}

bool RenderTrace::reason_selected(const char* reason) const
{
    return m_options.reason_filter.empty() ||
           contains(reason ? std::string_view(reason) : std::string_view{}, m_options.reason_filter);
}

std::string RenderTrace::prefix(TraceCategory category, const char* stage, const char* operation) const
{
    std::ostringstream out;
    out << "[rmlui-bgfx][trace][frame=" << m_frame_index << "][path="
        << trace_render_path_name(m_path) << "][cat=" << trace_category_name(category)
        << "][stage=" << (stage ? stage : "unknown") << "][op="
        << (operation ? operation : "unknown") << "]";
    return out.str();
}

void RenderTrace::remember(std::string line)
{
    if (m_options.ring_line_count == 0) {
        return;
    }
    while (m_ring.size() >= m_options.ring_line_count) {
        m_ring.pop_front();
    }
    m_ring.push_back(std::move(line));
}

RenderTraceLine::RenderTraceLine(RenderTrace& trace, TraceCategory category, const char* stage,
                                 const char* operation)
    : m_trace(trace), m_category(category), m_stage(stage), m_operation(operation)
{
}

RenderTraceLine& RenderTraceLine::field(const char* name, bool value)
{
    m_fields << ' ' << name << '=' << (value ? 1 : 0);
    return *this;
}

RenderTraceLine& RenderTraceLine::field(const char* name, const char* value)
{
    m_fields << ' ' << name << '=' << (value ? value : "<null>");
    return *this;
}

RenderTraceLine& RenderTraceLine::field(const char* name, std::string_view value)
{
    m_fields << ' ' << name << '=';
    m_fields.write(value.data(), static_cast<std::streamsize>(value.size()));
    return *this;
}

RenderTraceLine& RenderTraceLine::handle(const char* name, bgfx::TextureHandle handle)
{
    m_fields << ' ' << name << '=';
    if (bgfx::isValid(handle)) {
        m_fields << handle.idx;
    } else {
        m_fields << "invalid";
    }
    return *this;
}

RenderTraceLine& RenderTraceLine::handle(const char* name, bgfx::FrameBufferHandle handle)
{
    m_fields << ' ' << name << '=';
    if (bgfx::isValid(handle)) {
        m_fields << handle.idx;
    } else {
        m_fields << "invalid";
    }
    return *this;
}

RenderTraceLine& RenderTraceLine::fb_rect(const char* name, FbRect rect)
{
    m_fields << ' ' << name << "=fb(" << rect.x << ',' << rect.y << ' ' << rect.w << 'x'
             << rect.h << ')';
    return *this;
}

RenderTraceLine& RenderTraceLine::local_rect(const char* name, LocalFbRect rect)
{
    m_fields << ' ' << name << "=local(" << rect.x << ',' << rect.y << ' ' << rect.w << 'x'
             << rect.h << ')';
    return *this;
}

RenderTraceLine& RenderTraceLine::logical_rect(const char* name, LogicalRect rect)
{
    m_fields << ' ' << name << "=logical(" << rect.x << ',' << rect.y << ' ' << rect.w << 'x'
             << rect.h << ')';
    return *this;
}

RenderTraceLine& RenderTraceLine::rml_rect(const char* name, const Rml::Rectanglei& rect)
{
    m_fields << ' ' << name << "=rml(" << rect.Left() << ',' << rect.Top() << ' '
             << rect.Width() << 'x' << rect.Height() << ')';
    return *this;
}

RenderTraceLine& RenderTraceLine::scissor(const char* name, const ScissorState& scissor)
{
    if (!scissor.enabled) {
        m_fields << ' ' << name << "=off";
    } else {
        rml_rect(name, scissor.region);
    }
    return *this;
}

RenderTraceLine& RenderTraceLine::texture_region(const char* name, const TextureRegion& region)
{
    m_fields << ' ' << name << "={";
    handle("tex", region.texture);
    fb_rect("global", region.global_bounds);
    local_rect("local", region.local_rect);
    m_fields << " size=" << region.texture_width << 'x' << region.texture_height << '}';
    return *this;
}

RenderTraceLine& RenderTraceLine::pass_request(const RmlUiPassRequest& request)
{
    field("kind", trace_pass_kind_name(request.kind));
    field("reason", trace_pass_reason_name(request.reason));
    field("name", request.name ? request.name : "<null>");
    field("framebuffer_key", request.framebuffer);
    field("fb", request.bgfx_framebuffer_idx == std::numeric_limits<uint16_t>::max()
                    ? -1
                    : int(request.bgfx_framebuffer_idx));
    local_rect("view_rect", LocalFbRect{request.x, request.y, request.width, request.height});
    field("clear_color", request.clears_color);
    field("clear_stencil", request.clears_stencil);
    return *this;
}

RenderTraceLine& RenderTraceLine::pass(const RmlUiPass& pass)
{
    field("view", unsigned(pass.view));
    field("reused", pass.reused);
    pass_request(pass.request);
    return *this;
}

RenderTraceLine& RenderTraceLine::reason(const char* value)
{
    field("reason", value ? value : "<null>");
    return *this;
}

void RenderTraceLine::emit()
{
    if (m_emitted) {
        return;
    }
    m_trace.emit(m_category, m_stage, m_operation, m_fields.str());
    m_emitted = true;
}

} // namespace rmlui_bgfx
