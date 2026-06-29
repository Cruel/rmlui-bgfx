#include "rmlui_bgfx_planning.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <vector>

using namespace rmlui_bgfx;

namespace {

using Color = std::array<float, 4>;

void check_color(Color actual, Color expected)
{
    for (size_t i = 0; i < actual.size(); ++i) {
        CHECK(actual[i] == Catch::Approx(expected[i]).margin(0.0001f));
    }
}

Color css_color_matrix_expected(const std::array<float, 16>& m, Color rgba)
{
    const float alpha = rgba[3];
    return {
        rgba[0] * m[0] + rgba[1] * m[1] + rgba[2] * m[2] + alpha * m[3],
        rgba[0] * m[4] + rgba[1] * m[5] + rgba[2] * m[6] + alpha * m[7],
        rgba[0] * m[8] + rgba[1] * m[9] + rgba[2] * m[10] + alpha * m[11],
        alpha,
    };
}

} // namespace

TEST_CASE("RmlUi fullscreen triangle covers clip space with portable UVs")
{
    const auto top_left = fullscreen_triangle(false);
    CHECK(top_left[0].x == -1.0f);
    CHECK(top_left[1].x == 3.0f);
    CHECK(top_left[2].y == 3.0f);
    CHECK(top_left[0].v == 1.0f);
    CHECK(top_left[2].v == -1.0f);

    const auto bottom_left = fullscreen_triangle(true);
    CHECK(bottom_left[0].v == 0.0f);
    CHECK(bottom_left[1].v == 0.0f);
    CHECK(bottom_left[2].v == 2.0f);
}

TEST_CASE("RmlUi layer pool allocation is bounded by maximum nesting depth")
{
    LayerPoolPlan pool;
    constexpr uint32_t max_depth = 7;

    for (int frame = 0; frame < 10000; ++frame) {
        pool.begin_frame();
        for (uint32_t i = 0; i < max_depth; ++i) {
            CHECK(pool.push() == i + 1);
        }
    }

    CHECK(pool.slot_count() == max_depth + 1);
    CHECK(pool.allocation_count() == max_depth + 1);
}

TEST_CASE("RmlUi postprocess pool does not consume layer handles")
{
    LayerPoolPlan layers;
    PostprocessPoolPlan postprocess;

    layers.begin_frame();
    CHECK(layers.push() == 1);
    postprocess.mark_allocated(PostprocessTargetKind::Scratch);
    postprocess.mark_allocated(PostprocessTargetKind::Primary);
    CHECK(layers.push() == 2);
    CHECK(layers.slot_count() == 3);
    CHECK(postprocess.allocation_count() == 2);

    layers.begin_frame();
    CHECK(layers.push() == 1);
    CHECK(layers.push() == 2);
}

TEST_CASE("RmlUi postprocess roles stay independent like GL3 named framebuffers")
{
    PostprocessPoolPlan postprocess;

    postprocess.mark_allocated(PostprocessTargetKind::Primary);
    postprocess.mark_allocated(PostprocessTargetKind::Secondary);
    postprocess.mark_allocated(PostprocessTargetKind::Tertiary);
    postprocess.mark_allocated(PostprocessTargetKind::BlendMask);
    postprocess.mark_allocated(PostprocessTargetKind::Scratch);

    CHECK(postprocess.allocated(PostprocessTargetKind::Primary));
    CHECK(postprocess.allocated(PostprocessTargetKind::Secondary));
    CHECK(postprocess.allocated(PostprocessTargetKind::Tertiary));
    CHECK(postprocess.allocated(PostprocessTargetKind::BlendMask));
    CHECK(postprocess.allocated(PostprocessTargetKind::Scratch));
    CHECK(postprocess.allocation_count() == PostprocessPoolPlan::TargetCount);

    postprocess.mark_allocated(PostprocessTargetKind::Primary);
    postprocess.mark_allocated(PostprocessTargetKind::BlendMask);
    CHECK(postprocess.allocation_count() == PostprocessPoolPlan::TargetCount);
}

TEST_CASE("RmlUi resize bookkeeping recreates layer and postprocess resources independently")
{
    LayerPoolPlan layers;
    PostprocessPoolPlan postprocess;
    layers.begin_frame();
    CHECK(layers.push() == 1);
    CHECK(layers.push() == 2);
    postprocess.mark_allocated(PostprocessTargetKind::Scratch);
    postprocess.mark_allocated(PostprocessTargetKind::BlendMask);

    layers.reset_resources();
    postprocess.reset_resources();

    CHECK(layers.slot_count() == 1);
    CHECK(layers.allocation_count() == 1);
    CHECK_FALSE(postprocess.allocated(PostprocessTargetKind::Scratch));
    CHECK(postprocess.allocation_count() == 0);
}

TEST_CASE("RmlUi stencil planner never treats depth-only fallback as stencil")
{
    CHECK(choose_stencil_plan(true, false) == StencilPlan::D24S8);
    CHECK(choose_stencil_plan(false, true) == StencilPlan::D0S8);
    CHECK(choose_stencil_plan(false, false) == StencilPlan::Unsupported);
}

TEST_CASE("RmlUi base presentation policy preserves GL3 root-layer semantics")
{
    const auto direct = choose_base_presentation_policy(true, true, false, true, false);
    CHECK(direct.mode == BasePresentationMode::DirectToBackbuffer);
    CHECK(direct.fallback_reason == nullptr);

    const auto no_request = choose_base_presentation_policy(false, true, false, true, false);
    CHECK(no_request.mode == BasePresentationMode::Offscreen);
    REQUIRE(no_request.fallback_reason != nullptr);

    const auto no_capability = choose_base_presentation_policy(true, false, false, true, false);
    CHECK(no_capability.mode == BasePresentationMode::Offscreen);
    REQUIRE(no_capability.fallback_reason != nullptr);

    const auto no_stencil = choose_base_presentation_policy(true, true, false, false, false);
    CHECK(no_stencil.mode == BasePresentationMode::Offscreen);
    REQUIRE(no_stencil.fallback_reason != nullptr);

    const auto needs_root = choose_base_presentation_policy(true, true, true, true, false);
    CHECK(needs_root.mode == BasePresentationMode::Offscreen);
    REQUIRE(needs_root.fallback_reason != nullptr);

    const auto webgl_feedback = choose_base_presentation_policy(true, true, false, true, true);
    CHECK(webgl_feedback.mode == BasePresentationMode::Offscreen);
    REQUIRE(webgl_feedback.fallback_reason != nullptr);
}

TEST_CASE("RmlUi clip stencil planner matches GL3 Set/Intersect reference transitions")
{
    // GL3 Set and SetInverse both reset history through a broad clear, then write a replacement
    // value through geometry. Probe 23 remains the visual guard for stale stencil leakage.
    const auto set = plan_stencil_clip_operation(37, ClipOperationPlan::Set);
    CHECK(set.previous_ref == 1);
    CHECK(set.next_ref == 1);
    CHECK_FALSE(set.normalize_before_render);

    const auto inverse = plan_stencil_clip_operation(37, ClipOperationPlan::SetInverse);
    CHECK(inverse.previous_ref == 1);
    CHECK(inverse.next_ref == 1);
    CHECK_FALSE(inverse.normalize_before_render);

    const auto first_intersect = plan_stencil_clip_operation(1, ClipOperationPlan::Intersect);
    CHECK(first_intersect.previous_ref == 1);
    CHECK(first_intersect.next_ref == 2);
    CHECK_FALSE(first_intersect.normalize_before_render);

    const auto normal_intersect = plan_stencil_clip_operation(253, ClipOperationPlan::Intersect);
    CHECK(normal_intersect.previous_ref == 253);
    CHECK(normal_intersect.next_ref == 254);
    CHECK_FALSE(normal_intersect.normalize_before_render);

    const auto overflow_intersect = plan_stencil_clip_operation(254, ClipOperationPlan::Intersect);
    CHECK(overflow_intersect.previous_ref == 1);
    CHECK(overflow_intersect.next_ref == 2);
    CHECK(overflow_intersect.normalize_before_render);
}

TEST_CASE("RmlUi clip replay assumptions stay conservative")
{
    const auto bounded = plan_stencil_clip_operation(2, ClipOperationPlan::Intersect);
    CHECK(bounded.previous_ref == 2);
    CHECK(bounded.next_ref == 3);
    CHECK_FALSE(bounded.normalize_before_render);

    const auto reset = plan_stencil_clip_operation(1, ClipOperationPlan::Set);
    CHECK(reset.previous_ref == 1);
    CHECK(reset.next_ref == 1);
}

TEST_CASE("RmlUi gaussian kernel is normalized")
{
    const auto kernel = gaussian_kernel(4.0f);
    REQUIRE(kernel.weights.size() > 1);
    float sum = kernel.weights[0];
    for (size_t i = 1; i < kernel.weights.size(); ++i) {
        sum += 2.0f * kernel.weights[i];
    }
    CHECK(sum == Catch::Approx(1.0f).margin(0.0001f));
}

TEST_CASE("RmlUi color filter matrices match expected scalar behavior")
{
    const auto brightness = make_brightness_filter(2.0f);
    CHECK(brightness.kind == FilterKind::ColorMatrix);
    CHECK(brightness.matrix[0] == 2.0f);
    CHECK(brightness.matrix[5] == 2.0f);
    CHECK(brightness.matrix[10] == 2.0f);

    const auto invert = make_invert_filter(1.0f);
    CHECK(invert.matrix[0] == -1.0f);
    CHECK(invert.matrix[5] == -1.0f);
    CHECK(invert.matrix[10] == -1.0f);
    CHECK(invert.matrix[3] == 1.0f);
    CHECK(invert.matrix[7] == 1.0f);
    CHECK(invert.matrix[11] == 1.0f);
}

TEST_CASE("RmlUi texture ownership release policy only destroys owned texture handles")
{
    CHECK(texture_ownership_releases_handle(TextureOwnership::External));
    CHECK(texture_ownership_releases_handle(TextureOwnership::SavedLayer));
    CHECK_FALSE(texture_ownership_releases_handle(TextureOwnership::InternalLayerAttachment));
    CHECK_FALSE(texture_ownership_releases_handle(TextureOwnership::Postprocess));

    CHECK(mask_filter_owns_saved_texture(TextureOwnership::SavedLayer));
    CHECK_FALSE(mask_filter_owns_saved_texture(TextureOwnership::External));
    CHECK_FALSE(mask_filter_owns_saved_texture(TextureOwnership::InternalLayerAttachment));
    CHECK_FALSE(mask_filter_owns_saved_texture(TextureOwnership::Postprocess));
}

TEST_CASE("RmlUi filter simplifier removes no-op filters")
{
    CHECK(is_noop_filter(make_opacity_filter(1.0f)));
    CHECK(is_noop_filter(FilterRecord{.kind = FilterKind::Blur, .sigma = 0.0f}));
    CHECK(is_noop_filter(FilterRecord{.kind = FilterKind::Blur, .sigma = 0.49f}));
    CHECK(is_noop_filter(make_brightness_filter(1.0f)));
    CHECK(is_noop_filter(make_contrast_filter(1.0f)));
    CHECK(is_noop_filter(make_invert_filter(0.0f)));
    CHECK(is_noop_filter(make_grayscale_filter(0.0f)));
    CHECK(is_noop_filter(make_sepia_filter(0.0f)));
    CHECK(is_noop_filter(make_hue_rotate_filter(0.0f)));
    CHECK(is_noop_filter(make_saturate_filter(1.0f)));
}

TEST_CASE("RmlUi color matrix multiplication composes sequential filters")
{
    const auto first = make_brightness_filter(1.25f).matrix;
    const auto second = make_contrast_filter(0.8f).matrix;
    const auto combined = multiply_color_matrices(second, first);

    const Color premul{0.18f, 0.30f, 0.42f, 0.60f};
    const auto sequential = apply_color_matrix(second, apply_color_matrix(first, premul));
    const auto composed = apply_color_matrix(combined, premul);

    check_color(composed, sequential);
}

TEST_CASE("RmlUi filter simplifier collapses consecutive color matrices")
{
    const std::vector<FilterRecord> chain{
        make_brightness_filter(1.2f),
        make_contrast_filter(0.75f),
        FilterRecord{.kind = FilterKind::Opacity, .scalar = 1.0f},
        make_invert_filter(0.2f),
    };

    const auto simplified = simplify_filter_chain(chain);
    REQUIRE(simplified.size() == 1);
    CHECK(simplified[0].kind == FilterKind::ColorMatrix);
    CHECK_FALSE(is_identity_color_matrix(simplified[0].matrix));
}

TEST_CASE("RmlUi color-only filter chains can fold into final composite")
{
    const std::vector<FilterRecord> chain{
        make_brightness_filter(1.2f),
        make_opacity_filter(0.5f),
        make_contrast_filter(0.75f),
        make_opacity_filter(0.25f),
    };

    const auto simplified = simplify_filter_chain(chain);
    const ColorOnlyFilterPlan plan = plan_color_only_filter_chain(simplified);

    REQUIRE(plan.eligible);
    REQUIRE(plan.has_effect);
    CHECK(plan.opacity == Catch::Approx(0.125f));

    const Color premul{0.18f, 0.30f, 0.42f, 0.60f};
    Color sequential = premul;
    for (const FilterRecord& filter : simplified) {
        if (filter.kind == FilterKind::ColorMatrix) {
            sequential = apply_color_matrix(filter.matrix, sequential);
        }
    }
    const auto folded = apply_color_matrix(plan.matrix, premul);
    check_color(folded, sequential);
}

TEST_CASE("RmlUi color-only filter folding rejects texture-dependent filters")
{
    auto rejected_plan = [](FilterRecord filter) {
        std::vector<FilterRecord> chain{make_brightness_filter(1.1f), filter};
        return plan_color_only_filter_chain(simplify_filter_chain(chain));
    };

    FilterRecord blur;
    blur.kind = FilterKind::Blur;
    blur.sigma = 2.0f;
    CHECK_FALSE(rejected_plan(blur).eligible);

    FilterRecord drop_shadow;
    drop_shadow.kind = FilterKind::DropShadow;
    drop_shadow.sigma = 2.0f;
    CHECK_FALSE(rejected_plan(drop_shadow).eligible);

    FilterRecord mask;
    mask.kind = FilterKind::MaskImage;
    mask.mask_bounds = {0, 0, 10, 10};
    CHECK_FALSE(rejected_plan(mask).eligible);
}

TEST_CASE("RmlUi color matrix helper uses row-major RGB rows with translation")
{
    const auto identity = make_brightness_filter(1.0f);
    const std::array<float, 4> premul{0.2f, 0.4f, 0.1f, 0.5f};
    const auto unchanged = apply_color_matrix(identity.matrix, premul);
    CHECK(unchanged[0] == Catch::Approx(0.2f));
    CHECK(unchanged[1] == Catch::Approx(0.4f));
    CHECK(unchanged[2] == Catch::Approx(0.1f));
    CHECK(unchanged[3] == Catch::Approx(0.5f));

    const auto contrast = make_contrast_filter(2.0f);
    const auto contrasted = apply_color_matrix(contrast.matrix, {0.25f, 0.25f, 0.25f, 0.5f});
    CHECK(contrasted[0] == Catch::Approx(0.25f));
    CHECK(contrasted[1] == Catch::Approx(0.25f));
    CHECK(contrasted[2] == Catch::Approx(0.25f));
    CHECK(contrasted[3] == Catch::Approx(0.5f));
}

TEST_CASE("RmlUi premultiplied color matrices match GL3 behavior for translucent input")
{
    const Color premul{0.18f, 0.30f, 0.42f, 0.60f};

    SECTION("identity")
    {
        check_color(apply_color_matrix(make_brightness_filter(1.0f).matrix, premul), premul);
    }

    SECTION("brightness")
    {
        const auto filter = make_brightness_filter(1.25f);
        check_color(apply_color_matrix(filter.matrix, premul),
                    css_color_matrix_expected(filter.matrix, premul));
    }

    SECTION("contrast")
    {
        const auto filter = make_contrast_filter(1.40f);
        check_color(apply_color_matrix(filter.matrix, premul),
                    css_color_matrix_expected(filter.matrix, premul));
    }

    SECTION("invert")
    {
        const auto filter = make_invert_filter(0.75f);
        check_color(apply_color_matrix(filter.matrix, premul),
                    css_color_matrix_expected(filter.matrix, premul));
    }

    SECTION("grayscale")
    {
        const auto filter = make_grayscale_filter(0.65f);
        check_color(apply_color_matrix(filter.matrix, premul),
                    css_color_matrix_expected(filter.matrix, premul));
    }

    SECTION("sepia")
    {
        const auto filter = make_sepia_filter(0.80f);
        check_color(apply_color_matrix(filter.matrix, premul),
                    css_color_matrix_expected(filter.matrix, premul));
    }

    SECTION("hue rotate")
    {
        const auto filter = make_hue_rotate_filter(0.70f);
        check_color(apply_color_matrix(filter.matrix, premul),
                    css_color_matrix_expected(filter.matrix, premul));
    }

    SECTION("saturate")
    {
        const auto filter = make_saturate_filter(1.70f);
        check_color(apply_color_matrix(filter.matrix, premul),
                    css_color_matrix_expected(filter.matrix, premul));
    }
}
