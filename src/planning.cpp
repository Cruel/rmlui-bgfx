#include "rmlui_bgfx_planning.hpp"

#include <algorithm>
#include <cmath>

namespace rmlui_bgfx {

std::array<FullscreenVertex, 3> fullscreen_triangle(bool origin_bottom_left)
{
    const float v0 = origin_bottom_left ? 0.0f : 1.0f;
    const float v2 = origin_bottom_left ? 2.0f : -1.0f;
    return {{
        {-1.0f, -1.0f, 0.0f, v0},
        {3.0f, -1.0f, 2.0f, v0},
        {-1.0f, 3.0f, 0.0f, v2},
    }};
}

void LayerPoolPlan::begin_frame() { m_next_temporary = 1; }

uint32_t LayerPoolPlan::push()
{
    const uint32_t slot = m_next_temporary++;
    if (slot >= m_slot_count) {
        m_slot_count = slot + 1;
        ++m_allocation_count;
    }
    return slot;
}

void LayerPoolPlan::note_allocated(uint32_t slot)
{
    if (slot >= m_slot_count) {
        m_allocation_count += slot + 1 - m_slot_count;
        m_slot_count = slot + 1;
    }
}

void LayerPoolPlan::reset_resources()
{
    m_next_temporary = 1;
    m_slot_count = 1;
    m_allocation_count = 1;
}

static uint32_t postprocess_index(PostprocessTargetKind target)
{
    return static_cast<uint32_t>(target);
}

void PostprocessPoolPlan::mark_allocated(PostprocessTargetKind target)
{
    const uint32_t index = postprocess_index(target);
    if (index >= TargetCount)
        return;
    if (!m_allocated[index]) {
        m_allocated[index] = true;
        ++m_allocation_count;
    }
}

void PostprocessPoolPlan::reset_resources()
{
    m_allocated = {};
    m_allocation_count = 0;
}

bool PostprocessPoolPlan::allocated(PostprocessTargetKind target) const
{
    const uint32_t index = postprocess_index(target);
    return index < TargetCount && m_allocated[index];
}

StencilPlan choose_stencil_plan(bool d24s8_supported, bool d0s8_supported)
{
    if (d24s8_supported)
        return StencilPlan::D24S8;
    if (d0s8_supported)
        return StencilPlan::D0S8;
    return StencilPlan::Unsupported;
}

BasePresentationPolicy choose_base_presentation_policy(bool direct_mode_requested,
                                                       bool direct_mode_capable,
                                                       bool root_requires_preservation,
                                                       bool stencil_capable,
                                                       bool webgl_feedback_sensitive)
{
    if (!direct_mode_requested) {
        return {BasePresentationMode::Offscreen, "direct mode not requested"};
    }
    if (!direct_mode_capable) {
        return {BasePresentationMode::Offscreen, "direct mode not capable"};
    }
    if (root_requires_preservation) {
        return {BasePresentationMode::Offscreen, "root requires offscreen preservation"};
    }
    if (!stencil_capable) {
        return {BasePresentationMode::Offscreen, "stencil path unavailable"};
    }
    if (webgl_feedback_sensitive) {
        return {BasePresentationMode::Offscreen, "webgl feedback-loop sensitive"};
    }
    return {BasePresentationMode::DirectToBackbuffer, nullptr};
}

StencilClipPlan plan_stencil_clip_operation(uint8_t current_ref, ClipOperationPlan operation)
{
    StencilClipPlan plan;
    plan.previous_ref = current_ref;
    plan.next_ref = current_ref;

    switch (operation) {
    case ClipOperationPlan::Set:
    case ClipOperationPlan::SetInverse:
        plan.previous_ref = 1;
        plan.next_ref = 1;
        break;
    case ClipOperationPlan::Intersect:
        if (current_ref == 254) {
            plan.previous_ref = 1;
            plan.next_ref = 2;
            plan.normalize_before_render = true;
        } else {
            plan.next_ref = uint8_t(current_ref + 1);
        }
        break;
    }

    return plan;
}

GaussianKernel gaussian_kernel(float sigma)
{
    GaussianKernel kernel;
    if (sigma < 0.5f) {
        kernel.weights = {1.0f};
        return kernel;
    }

    const int radius = std::clamp(int(std::ceil(sigma * 3.0f)), 1, 31);
    kernel.weights.resize(size_t(radius + 1));
    const float denom = 2.0f * sigma * sigma;
    float sum = 0.0f;
    for (int i = 0; i <= radius; ++i) {
        const float weight = std::exp(-(float(i * i) / denom));
        kernel.weights[size_t(i)] = weight;
        sum += (i == 0) ? weight : 2.0f * weight;
    }
    for (float& weight : kernel.weights) {
        weight /= sum;
    }
    return kernel;
}

static std::array<float, 16> identity() { return identity_color_matrix(); }

std::array<float, 16> identity_color_matrix()
{
    return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}

bool is_identity_color_matrix(const std::array<float, 16>& matrix)
{
    return matrix == identity_color_matrix();
}

std::array<float, 16> multiply_color_matrices(const std::array<float, 16>& lhs,
                                              const std::array<float, 16>& rhs)
{
    std::array<float, 16> result{};
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            float value = 0.0f;
            for (int k = 0; k < 4; ++k) {
                value += lhs[row * 4 + k] * rhs[k * 4 + col];
            }
            result[row * 4 + col] = value;
        }
    }
    return result;
}

bool is_noop_filter(const FilterRecord& filter)
{
    switch (filter.kind) {
    case FilterKind::Opacity:
        return filter.scalar == 1.0f;
    case FilterKind::Blur:
        return filter.sigma < 0.5f;
    case FilterKind::ColorMatrix:
        return is_identity_color_matrix(filter.matrix);
    case FilterKind::DropShadow:
    case FilterKind::MaskImage:
    case FilterKind::Invalid:
        return false;
    }
    return false;
}

std::vector<FilterRecord> simplify_filter_chain(std::span<const FilterRecord> filters)
{
    std::vector<FilterRecord> simplified;
    simplified.reserve(filters.size());
    std::array<float, 16> pending_matrix = identity();
    bool have_pending_matrix = false;

    auto flush_matrix = [&]() {
        if (!have_pending_matrix)
            return;
        FilterRecord matrix_filter;
        matrix_filter.kind = FilterKind::ColorMatrix;
        matrix_filter.matrix = pending_matrix;
        if (!is_identity_color_matrix(matrix_filter.matrix))
            simplified.push_back(matrix_filter);
        have_pending_matrix = false;
        pending_matrix = identity();
    };

    for (const FilterRecord& filter : filters) {
        if (is_noop_filter(filter))
            continue;
        if (filter.kind == FilterKind::ColorMatrix) {
            pending_matrix = have_pending_matrix
                                 ? multiply_color_matrices(filter.matrix, pending_matrix)
                                 : filter.matrix;
            have_pending_matrix = true;
            continue;
        }
        flush_matrix();
        simplified.push_back(filter);
    }

    flush_matrix();
    return simplified;
}

ColorOnlyFilterPlan plan_color_only_filter_chain(std::span<const FilterRecord> filters)
{
    ColorOnlyFilterPlan plan;
    plan.eligible = true;
    plan.opacity = 1.0f;
    plan.matrix = identity_color_matrix();

    for (const FilterRecord& filter : filters) {
        if (is_noop_filter(filter)) {
            continue;
        }
        switch (filter.kind) {
        case FilterKind::Opacity:
            plan.opacity *= filter.scalar;
            plan.has_effect = true;
            break;
        case FilterKind::ColorMatrix:
            plan.matrix = multiply_color_matrices(filter.matrix, plan.matrix);
            plan.has_effect = true;
            break;
        case FilterKind::Blur:
        case FilterKind::DropShadow:
        case FilterKind::MaskImage:
        case FilterKind::Invalid:
            plan.eligible = false;
            return plan;
        }
    }

    if (!plan.has_effect) {
        plan.eligible = false;
    }
    return plan;
}

static FilterRecord color_matrix(std::array<float, 16> matrix)
{
    FilterRecord record;
    record.kind = FilterKind::ColorMatrix;
    record.matrix = matrix;
    return record;
}

FilterRecord make_opacity_filter(float value)
{
    FilterRecord record;
    record.kind = FilterKind::Opacity;
    record.scalar = value;
    return record;
}

FilterRecord make_brightness_filter(float value)
{
    auto m = identity();
    m[0] = value;
    m[5] = value;
    m[10] = value;
    return color_matrix(m);
}

FilterRecord make_contrast_filter(float value)
{
    auto m = identity();
    const float grayness = 0.5f - 0.5f * value;
    m[0] = value;
    m[5] = value;
    m[10] = value;
    m[3] = grayness;
    m[7] = grayness;
    m[11] = grayness;
    return color_matrix(m);
}

FilterRecord make_invert_filter(float value)
{
    value = std::clamp(value, 0.0f, 1.0f);
    auto m = identity();
    const float inverted = 1.0f - 2.0f * value;
    m[0] = inverted;
    m[5] = inverted;
    m[10] = inverted;
    m[3] = value;
    m[7] = value;
    m[11] = value;
    return color_matrix(m);
}

FilterRecord make_grayscale_filter(float value)
{
    const float rev = 1.0f - value;
    const std::array<float, 3> r{0.2126f, 0.7152f, 0.0722f};
    const std::array<float, 3> g{0.2126f, 0.7152f, 0.0722f};
    const std::array<float, 3> b{0.2126f, 0.7152f, 0.0722f};
    return color_matrix({
        r[0] * value + rev,
        r[1] * value,
        r[2] * value,
        0,
        g[0] * value,
        g[1] * value + rev,
        g[2] * value,
        0,
        b[0] * value,
        b[1] * value,
        b[2] * value + rev,
        0,
        0,
        0,
        0,
        1,
    });
}

FilterRecord make_sepia_filter(float value)
{
    const float rev = 1.0f - value;
    return color_matrix({
        0.393f * value + rev,
        0.769f * value,
        0.189f * value,
        0,
        0.349f * value,
        0.686f * value + rev,
        0.168f * value,
        0,
        0.272f * value,
        0.534f * value,
        0.131f * value + rev,
        0,
        0,
        0,
        0,
        1,
    });
}

FilterRecord make_hue_rotate_filter(float radians)
{
    const float s = std::sin(radians);
    const float c = std::cos(radians);
    return color_matrix({
        0.213f + 0.787f * c - 0.213f * s,
        0.715f - 0.715f * c - 0.715f * s,
        0.072f - 0.072f * c + 0.928f * s,
        0,
        0.213f - 0.213f * c + 0.143f * s,
        0.715f + 0.285f * c + 0.140f * s,
        0.072f - 0.072f * c - 0.283f * s,
        0,
        0.213f - 0.213f * c - 0.787f * s,
        0.715f - 0.715f * c + 0.715f * s,
        0.072f + 0.928f * c + 0.072f * s,
        0,
        0,
        0,
        0,
        1,
    });
}

FilterRecord make_saturate_filter(float value)
{
    return color_matrix({
        0.213f + 0.787f * value,
        0.715f - 0.715f * value,
        0.072f - 0.072f * value,
        0,
        0.213f - 0.213f * value,
        0.715f + 0.285f * value,
        0.072f - 0.072f * value,
        0,
        0.213f - 0.213f * value,
        0.715f - 0.715f * value,
        0.072f + 0.928f * value,
        0,
        0,
        0,
        0,
        1,
    });
}

std::array<float, 4> apply_color_matrix(const std::array<float, 16>& m, std::array<float, 4> rgba)
{
    const float alpha = rgba[3];
    return {
        rgba[0] * m[0] + rgba[1] * m[1] + rgba[2] * m[2] + alpha * m[3],
        rgba[0] * m[4] + rgba[1] * m[5] + rgba[2] * m[6] + alpha * m[7],
        rgba[0] * m[8] + rgba[1] * m[9] + rgba[2] * m[10] + alpha * m[11],
        alpha,
    };
}

GradientRecord make_invalid_gradient() { return {}; }

} // namespace rmlui_bgfx
