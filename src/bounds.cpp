#include "rmlui_bgfx_bounds.hpp"

#include <RmlUi/Core/Types.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace rmlui_bgfx {

namespace {

bool finite(float value) { return std::isfinite(value); }

bool finite(LogicalRect rect)
{
    return finite(rect.x) && finite(rect.y) && finite(rect.w) && finite(rect.h);
}

bool finite(Rml::Vector2f value) { return finite(value.x) && finite(value.y); }

LogicalRect rect_from_extents(float min_x, float min_y, float max_x, float max_y)
{
    if (!finite(min_x) || !finite(min_y) || !finite(max_x) || !finite(max_y) || max_x <= min_x ||
        max_y <= min_y) {
        return {};
    }
    return {min_x, min_y, max_x - min_x, max_y - min_y};
}

} // namespace

// ---------------------------------------------------------------------------
// FbRect helpers
// ---------------------------------------------------------------------------

uint64_t area(FbRect r)
{
    if (r.w <= 0 || r.h <= 0)
        return 0;
    return uint64_t(r.w) * uint64_t(r.h);
}

bool is_empty(FbRect r) { return r.w <= 0 || r.h <= 0; }

FbRect intersect(FbRect a, FbRect b)
{
    const int l = std::max(a.x, b.x);
    const int t = std::max(a.y, b.y);
    const int r = std::min(a.x + a.w, b.x + b.w);
    const int b2 = std::min(a.y + a.h, b.y + b.h);
    if (r <= l || b2 <= t)
        return {0, 0, 0, 0};
    return {l, t, r - l, b2 - t};
}

FbRect union_rects(FbRect a, FbRect b)
{
    if (is_empty(a))
        return b;
    if (is_empty(b))
        return a;
    const int l = std::min(a.x, b.x);
    const int t = std::min(a.y, b.y);
    const int r = std::max(a.x + a.w, b.x + b.w);
    const int b2 = std::max(a.y + a.h, b.y + b.h);
    return {l, t, r - l, b2 - t};
}

FbRect inflate(FbRect r, int x, int y)
{
    if (is_empty(r))
        return r;
    const int l = r.x - x;
    const int t = r.y - y;
    const int ri = r.x + r.w + x;
    const int b2 = r.y + r.h + y;
    if (ri <= l || b2 <= t)
        return {0, 0, 0, 0};
    return {l, t, ri - l, b2 - t};
}

FbRect clamp_to_surface(FbRect r, const SurfaceMetrics& surface)
{
    const int sw = std::max(surface.framebuffer_width, 0);
    const int sh = std::max(surface.framebuffer_height, 0);
    const int l = std::clamp(r.x, 0, sw);
    const int t = std::clamp(r.y, 0, sh);
    const int ri = std::clamp(r.x + r.w, 0, sw);
    const int b2 = std::clamp(r.y + r.h, 0, sh);
    if (ri <= l || b2 <= t)
        return {0, 0, 0, 0};
    return {l, t, ri - l, b2 - t};
}

FbRect align_outward_for_render_target(FbRect r)
{
    if (r.w <= 0 || r.h <= 0)
        return {0, 0, 0, 0};
    return {r.x, r.y, std::max(r.w, 1), std::max(r.h, 1)};
}

bool intersects(FbRect a, FbRect b) { return !is_empty(intersect(a, b)); }

bool intersects(Rml::Rectanglei a, FbRect b)
{
    return intersects(FbRect{a.Left(), a.Top(), a.Width(), a.Height()}, b);
}

// ---------------------------------------------------------------------------
// LogicalRect helpers
// ---------------------------------------------------------------------------

float area(LogicalRect r)
{
    if (r.w <= 0.0f || r.h <= 0.0f)
        return 0.0f;
    return r.w * r.h;
}

bool is_empty(LogicalRect r) { return r.w <= 0.0f || r.h <= 0.0f; }

LogicalRect intersect(LogicalRect a, LogicalRect b)
{
    const float l = std::max(a.x, b.x);
    const float t = std::max(a.y, b.y);
    const float r = std::min(a.x + a.w, b.x + b.w);
    const float b2 = std::min(a.y + a.h, b.y + b.h);
    if (r <= l || b2 <= t)
        return {0.0f, 0.0f, 0.0f, 0.0f};
    return {l, t, r - l, b2 - t};
}

LogicalRect union_rects(LogicalRect a, LogicalRect b)
{
    if (is_empty(a))
        return b;
    if (is_empty(b))
        return a;
    const float l = std::min(a.x, b.x);
    const float t = std::min(a.y, b.y);
    const float r = std::max(a.x + a.w, b.x + b.w);
    const float b2 = std::max(a.y + a.h, b.y + b.h);
    return {l, t, r - l, b2 - t};
}

LogicalRect inflate(LogicalRect r, float x, float y)
{
    if (is_empty(r))
        return r;
    const float l = r.x - x;
    const float t = r.y - y;
    const float ri = r.x + r.w + x;
    const float b2 = r.y + r.h + y;
    if (ri <= l || b2 <= t)
        return {0.0f, 0.0f, 0.0f, 0.0f};
    return {l, t, ri - l, b2 - t};
}

// ---------------------------------------------------------------------------
// Coordinate conversion
// ---------------------------------------------------------------------------

FbRect logical_to_framebuffer(LogicalRect logical, const SurfaceMetrics& surface)
{
    const SurfaceMetrics s = sanitize_surface_metrics(surface);
    const int left = int(std::floor(logical.x * s.scale_x));
    const int top = int(std::floor(logical.y * s.scale_y));
    const int right = int(std::ceil((logical.x + logical.w) * s.scale_x));
    const int bottom = int(std::ceil((logical.y + logical.h) * s.scale_y));
    if (right <= left || bottom <= top) {
        return {0, 0, 0, 0};
    }
    return {left, top, right - left, bottom - top};
}

LogicalRect framebuffer_to_logical(FbRect fb, const SurfaceMetrics& surface)
{
    const SurfaceMetrics s = sanitize_surface_metrics(surface);
    return {float(fb.x) / s.scale_x, float(fb.y) / s.scale_y, float(fb.w) / s.scale_x,
            float(fb.h) / s.scale_y};
}

// ---------------------------------------------------------------------------
// Geometry bounds
// ---------------------------------------------------------------------------

GeometryBoundsResult compute_indexed_geometry_bounds(Rml::Span<const Rml::Vertex> vertices,
                                                     Rml::Span<const int> indices)
{
    GeometryBoundsResult result;
    if (vertices.empty() || indices.empty()) {
        result.status = GeometryBoundsStatus::EmptyGeometry;
        return result;
    }

    float min_x = std::numeric_limits<float>::infinity();
    float min_y = std::numeric_limits<float>::infinity();
    float max_x = -std::numeric_limits<float>::infinity();
    float max_y = -std::numeric_limits<float>::infinity();

    bool saw_vertex = false;
    for (const int index : indices) {
        if (index < 0 || size_t(index) >= vertices.size()) {
            result.status = GeometryBoundsStatus::InvalidIndex;
            return result;
        }
        const Rml::Vector2f position = vertices[size_t(index)].position;
        if (!finite(position)) {
            result.status = GeometryBoundsStatus::NonFiniteVertex;
            return result;
        }
        saw_vertex = true;
        min_x = std::min(min_x, position.x);
        min_y = std::min(min_y, position.y);
        max_x = std::max(max_x, position.x);
        max_y = std::max(max_y, position.y);
    }

    if (!saw_vertex) {
        result.status = GeometryBoundsStatus::EmptyGeometry;
        return result;
    }

    result.logical = rect_from_extents(min_x, min_y, max_x, max_y);
    if (is_empty(result.logical)) {
        result.status = GeometryBoundsStatus::EmptyGeometry;
        return result;
    }

    result.status = GeometryBoundsStatus::Valid;
    return result;
}

GeometryBoundsResult compute_transformed_geometry_bounds(LogicalRect local_bounds,
                                                         Rml::Vector2f translation,
                                                         const Rml::Matrix4f* transform,
                                                         const SurfaceMetrics& surface)
{
    GeometryBoundsResult result;
    if (is_empty(local_bounds)) {
        result.status = GeometryBoundsStatus::EmptyGeometry;
        return result;
    }
    if (!finite(local_bounds)) {
        result.status = GeometryBoundsStatus::NonFiniteBounds;
        return result;
    }
    if (!finite(translation)) {
        result.status = GeometryBoundsStatus::NonFiniteTranslation;
        return result;
    }

    if (transform) {
        const float* data = transform->data();
        for (int i = 0; i < 16; ++i) {
            if (!finite(data[i])) {
                result.status = GeometryBoundsStatus::NonFiniteTransform;
                return result;
            }
        }
    }

    const std::array<Rml::Vector4f, 4> corners{{
        {local_bounds.x + translation.x, local_bounds.y + translation.y, 0.0f, 1.0f},
        {local_bounds.x + local_bounds.w + translation.x, local_bounds.y + translation.y, 0.0f,
         1.0f},
        {local_bounds.x + translation.x, local_bounds.y + local_bounds.h + translation.y, 0.0f,
         1.0f},
        {local_bounds.x + local_bounds.w + translation.x,
         local_bounds.y + local_bounds.h + translation.y, 0.0f, 1.0f},
    }};

    float min_x = std::numeric_limits<float>::infinity();
    float min_y = std::numeric_limits<float>::infinity();
    float max_x = -std::numeric_limits<float>::infinity();
    float max_y = -std::numeric_limits<float>::infinity();

    for (const Rml::Vector4f& corner : corners) {
        const Rml::Vector4f transformed = transform ? ((*transform) * corner) : corner;
        if (!finite(transformed.x) || !finite(transformed.y) || !finite(transformed.w) ||
            transformed.w == 0.0f) {
            result.status = GeometryBoundsStatus::NonFiniteOutput;
            return result;
        }
        const float inv_w = 1.0f / transformed.w;
        const float x = transformed.x * inv_w;
        const float y = transformed.y * inv_w;
        if (!finite(x) || !finite(y)) {
            result.status = GeometryBoundsStatus::NonFiniteOutput;
            return result;
        }
        min_x = std::min(min_x, x);
        min_y = std::min(min_y, y);
        max_x = std::max(max_x, x);
        max_y = std::max(max_y, y);
    }

    result.logical = rect_from_extents(min_x, min_y, max_x, max_y);
    if (is_empty(result.logical)) {
        result.status = GeometryBoundsStatus::EmptyGeometry;
        return result;
    }
    result.framebuffer = clamp_to_surface(logical_to_framebuffer(result.logical, surface), surface);
    if (is_empty(result.framebuffer)) {
        result.status = GeometryBoundsStatus::EmptyGeometry;
        return result;
    }
    result.status = GeometryBoundsStatus::Valid;
    return result;
}

// ---------------------------------------------------------------------------
// UV calculation
// ---------------------------------------------------------------------------

std::array<float, 4> uv_rect_for_source_region(FbRect source_region, int texture_width,
                                               int texture_height)
{
    if (texture_width <= 0 || texture_height <= 0)
        return {0.0f, 0.0f, 0.0f, 0.0f};
    const float tw = float(texture_width);
    const float th = float(texture_height);
    return {float(source_region.x) / tw, float(source_region.y) / th,
            float(source_region.x + source_region.w) / tw,
            float(source_region.y + source_region.h) / th};
}

std::array<float, 4> compute_mask_uv_transform(FbRect shaded_work_bounds, FbRect mask_bounds)
{
    const float inv_mask_w = 1.0f / float(std::max(mask_bounds.w, 1));
    const float inv_mask_h = 1.0f / float(std::max(mask_bounds.h, 1));
    return {
        float(shaded_work_bounds.w) * inv_mask_w,
        float(shaded_work_bounds.h) * inv_mask_h,
        float(shaded_work_bounds.x - mask_bounds.x) * inv_mask_w,
        float(shaded_work_bounds.y - mask_bounds.y) * inv_mask_h,
    };
}

// ---------------------------------------------------------------------------
// Filter expansion
// ---------------------------------------------------------------------------

FilterExpansion blur_expansion(float sigma)
{
    if (sigma <= 0.0f)
        return {};
    const int radius = int(std::ceil(sigma * 3.0f));
    return {radius, radius, radius, radius};
}

FilterExpansion drop_shadow_expansion(float sigma, float offset_x, float offset_y)
{
    FilterExpansion exp;
    const int blur_radius = (sigma > 0.0f) ? int(std::ceil(sigma * 3.0f)) : 0;
    exp.left = std::max(0, blur_radius + (offset_x < 0.0f ? int(std::ceil(-offset_x)) : 0));
    exp.top = std::max(0, blur_radius + (offset_y < 0.0f ? int(std::ceil(-offset_y)) : 0));
    exp.right = std::max(0, blur_radius + (offset_x > 0.0f ? int(std::ceil(offset_x)) : 0));
    exp.bottom = std::max(0, blur_radius + (offset_y > 0.0f ? int(std::ceil(offset_y)) : 0));
    return exp;
}

FilterExpansion max_expansions(const FilterExpansion& a, const FilterExpansion& b)
{
    return {std::max(a.left, b.left), std::max(a.top, b.top), std::max(a.right, b.right),
            std::max(a.bottom, b.bottom)};
}

FilterExpansion add_expansions(const FilterExpansion& a, const FilterExpansion& b)
{
    return {a.left + b.left, a.top + b.top, a.right + b.right, a.bottom + b.bottom};
}

FilterExpansion filter_chain_expansion(std::span<const FilterExpansion> expansions)
{
    FilterExpansion total;
    for (const auto& exp : expansions) {
        total = add_expansions(total, exp);
    }
    return total;
}

FbRect expand_bounds(FbRect r, const FilterExpansion& expansion)
{
    if (is_empty(r))
        return r;
    const int l = r.x - expansion.left;
    const int t = r.y - expansion.top;
    const int ri = r.x + r.w + expansion.right;
    const int b2 = r.y + r.h + expansion.bottom;
    if (ri <= l || b2 <= t)
        return {0, 0, 0, 0};
    return {l, t, ri - l, b2 - t};
}

FbRect apply_mask_constraints(FbRect draw_bounds, const FbRect* scissor_bounds,
                              const ConservativeMaskBounds* mask_bounds)
{
    if (is_empty(draw_bounds))
        return {};
    FbRect constrained = draw_bounds;
    if (scissor_bounds) {
        constrained = intersect(constrained, *scissor_bounds);
    }
    if (mask_bounds && mask_bounds->active) {
        constrained = intersect(constrained, mask_bounds->bounds);
    }
    return constrained;
}

ConservativeMaskBounds update_conservative_mask_bounds(ConservativeMaskBounds current,
                                                       Rml::ClipMaskOperation operation,
                                                       FbRect mask_geometry_bounds,
                                                       FbRect inverse_fallback_bounds)
{
    ConservativeMaskBounds next;
    switch (operation) {
    case Rml::ClipMaskOperation::Set:
        next.bounds = mask_geometry_bounds;
        next.active = true;
        next.inverse_fallback = false;
        return next;
    case Rml::ClipMaskOperation::Intersect:
        next.bounds =
            current.active ? intersect(current.bounds, mask_geometry_bounds) : mask_geometry_bounds;
        next.active = true;
        next.inverse_fallback = current.inverse_fallback;
        return next;
    case Rml::ClipMaskOperation::SetInverse:
        next.bounds = inverse_fallback_bounds;
        next.active = !is_empty(inverse_fallback_bounds);
        next.inverse_fallback = next.active;
        return next;
    }
    return current;
}

RenderBounds compute_child_layer_bounds(const SurfaceMetrics& surface,
                                        const RenderBounds* parent_bounds,
                                        const FbRect* scissor_region, bool transform_valid)
{
    RenderBounds bounds;

    const FbRect surface_fb{0, 0, std::max(surface.framebuffer_width, 0),
                            std::max(surface.framebuffer_height, 0)};
    const LogicalRect surface_logical{0.0f, 0.0f, float(surface.logical_width),
                                      float(surface.logical_height)};

    if (transform_valid || !scissor_region || is_empty(*scissor_region)) {
        bounds.framebuffer = surface_fb;
        bounds.logical = surface_logical;
    } else {
        bounds.framebuffer = clamp_to_surface(*scissor_region, surface);
        bounds.logical = framebuffer_to_logical(bounds.framebuffer, surface);
    }

    if (parent_bounds) {
        bounds.framebuffer = intersect(bounds.framebuffer, parent_bounds->framebuffer);
        bounds.logical = framebuffer_to_logical(bounds.framebuffer, surface);
    }

    if (bounds.framebuffer.w <= 0 || bounds.framebuffer.h <= 0) {
        bounds.framebuffer = {0, 0, 1, 1};
        bounds.logical = framebuffer_to_logical(bounds.framebuffer, surface);
    }

    return bounds;
}

} // namespace rmlui_bgfx
