#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace rmlui_bgfx {

struct FullscreenVertex {
    float x;
    float y;
    float u;
    float v;
};

[[nodiscard]] std::array<FullscreenVertex, 3> fullscreen_triangle(bool origin_bottom_left);

class LayerPoolPlan {
public:
    static constexpr uint32_t BaseLayer = 0;
    static constexpr uint32_t InvalidLayer = UINT32_MAX;

    void begin_frame();
    [[nodiscard]] uint32_t push();
    void note_allocated(uint32_t slot);
    void reset_resources();
    [[nodiscard]] uint32_t allocation_count() const { return m_allocation_count; }
    [[nodiscard]] uint32_t slot_count() const { return m_slot_count; }
    [[nodiscard]] uint32_t next_temporary() const { return m_next_temporary; }

private:
    uint32_t m_next_temporary = 1;
    uint32_t m_slot_count = 1;
    uint32_t m_allocation_count = 1;
};

// Semantic postprocess roles mirror RmlUi's GL3 backend: primary is the resolved/filter
// source-destination, secondary and tertiary are ordered ping-pong/filter temporaries, blend-mask
// owns SaveLayerAsMaskImage output, and scratch is for explicit feedback-loop copies. These are
// not anonymous interchangeable cache slots even when the optimized path uses bounded targets.
enum class PostprocessTargetKind {
    Primary,
    Secondary,
    Tertiary,
    BlendMask,
    Scratch,
};

class PostprocessPoolPlan {
public:
    static constexpr uint32_t TargetCount = 5;

    void mark_allocated(PostprocessTargetKind target);
    void reset_resources();
    [[nodiscard]] bool allocated(PostprocessTargetKind target) const;
    [[nodiscard]] uint32_t allocation_count() const { return m_allocation_count; }

private:
    std::array<bool, TargetCount> m_allocated{};
    uint32_t m_allocation_count = 0;
};

// Ordinary external/generated textures and SaveLayerAsTexture results release their handles
// through RmlUi texture lifetime. Layer attachments and postprocess targets are renderer-owned;
// SaveLayerAsMaskImage is a filter/blend-mask resource, not ordinary saved texture ownership.
enum class TextureOwnership {
    External,
    SavedLayer,
    InternalLayerAttachment,
    Postprocess,
};

enum class TargetLifetime {
    Frame,
    Viewport,
    External,
};

enum class TargetRole {
    LayerColorDepth,
    Postprocess,
};

[[nodiscard]] const char* target_lifetime_name(TargetLifetime lifetime);
[[nodiscard]] const char* target_role_name(TargetRole role);
[[nodiscard]] const char* postprocess_target_kind_name(PostprocessTargetKind kind);

[[nodiscard]] constexpr bool texture_ownership_releases_handle(TextureOwnership ownership)
{
    return ownership == TextureOwnership::External || ownership == TextureOwnership::SavedLayer;
}

[[nodiscard]] constexpr bool mask_filter_owns_saved_texture(TextureOwnership ownership)
{
    return ownership == TextureOwnership::SavedLayer;
}

enum class StencilPlan {
    D24S8,
    D0S8,
    StencilAttachment,
    Unsupported,
};

[[nodiscard]] StencilPlan choose_stencil_plan(bool d24s8_supported, bool d0s8_supported);

enum class BasePresentationMode {
    Offscreen,
    DirectToBackbuffer,
};

struct BasePresentationPolicy {
    BasePresentationMode mode = BasePresentationMode::Offscreen;
    const char* fallback_reason = nullptr;
};

[[nodiscard]] BasePresentationPolicy
choose_base_presentation_policy(bool direct_mode_requested, bool direct_mode_capable,
                                bool root_requires_preservation, bool stencil_capable,
                                bool webgl_feedback_sensitive);

enum class ClipOperationPlan {
    Set,
    SetInverse,
    Intersect,
};

// GL3 treats Set/SetInverse as broad stencil resets followed by replacement writes, while
// Intersect increments the active reference. Overflow normalization keeps that history explicit
// before continuing at a safe low reference value.
struct StencilClipPlan {
    uint8_t previous_ref = 1;
    uint8_t next_ref = 1;
    bool normalize_before_render = false;
};

[[nodiscard]] StencilClipPlan plan_stencil_clip_operation(uint8_t current_ref,
                                                          ClipOperationPlan operation);

struct GaussianKernel {
    std::vector<float> weights;
};

[[nodiscard]] GaussianKernel gaussian_kernel(float sigma);

enum class FilterKind {
    Invalid,
    Opacity,
    Blur,
    DropShadow,
    ColorMatrix,
    MaskImage,
};

struct FilterRecord {
    FilterKind kind = FilterKind::Invalid;
    float scalar = 1.0f;
    float sigma = 0.0f;
    std::array<float, 2> offset{};
    std::array<float, 4> color{};
    std::array<float, 16> matrix{};
    std::array<int, 4> mask_bounds{};
    uint64_t resource = 0;
};

struct ColorOnlyFilterPlan {
    bool eligible = false;
    bool has_effect = false;
    float opacity = 1.0f;
    std::array<float, 16> matrix{};
};

[[nodiscard]] bool is_identity_color_matrix(const std::array<float, 16>& matrix);
[[nodiscard]] std::array<float, 16> identity_color_matrix();
[[nodiscard]] std::array<float, 16> multiply_color_matrices(const std::array<float, 16>& lhs,
                                                            const std::array<float, 16>& rhs);
[[nodiscard]] bool is_noop_filter(const FilterRecord& filter);
[[nodiscard]] std::vector<FilterRecord>
simplify_filter_chain(std::span<const FilterRecord> filters);
[[nodiscard]] ColorOnlyFilterPlan
plan_color_only_filter_chain(std::span<const FilterRecord> filters);

[[nodiscard]] FilterRecord make_opacity_filter(float value);
[[nodiscard]] FilterRecord make_brightness_filter(float value);
[[nodiscard]] FilterRecord make_contrast_filter(float value);
[[nodiscard]] FilterRecord make_invert_filter(float value);
[[nodiscard]] FilterRecord make_grayscale_filter(float value);
[[nodiscard]] FilterRecord make_sepia_filter(float value);
[[nodiscard]] FilterRecord make_hue_rotate_filter(float radians);
[[nodiscard]] FilterRecord make_saturate_filter(float value);
// Matrix storage is row-major. Vectors are treated as columns. The fourth
// column stores RGB constants and is multiplied by source alpha for
// premultiplied-alpha parity with RmlUi's GL3 renderer. Alpha is preserved.
[[nodiscard]] std::array<float, 4> apply_color_matrix(const std::array<float, 16>& row_major_matrix,
                                                      std::array<float, 4> rgba);

enum class GradientKind {
    Invalid,
    Linear,
    RepeatingLinear,
    Radial,
    RepeatingRadial,
    Conic,
    RepeatingConic,
};

struct GradientStop {
    float position = 0.0f;
    std::array<float, 4> color{};
};

struct GradientRecord {
    GradientKind kind = GradientKind::Invalid;
    std::array<float, 4> p_v{};
    std::array<GradientStop, 16> stops{};
    uint32_t stop_count = 0;
};

[[nodiscard]] GradientRecord make_invalid_gradient();

} // namespace rmlui_bgfx
