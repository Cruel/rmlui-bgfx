#include "rmlui_bgfx_bounds.hpp"
#include "rmlui_bgfx_mapping.hpp"
#include "rmlui_bgfx_types.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

using namespace rmlui_bgfx;

namespace {

[[nodiscard]] SurfaceMetrics make_surface_metrics(int logical_width, int logical_height,
                                                  int framebuffer_width, int framebuffer_height)
{
    SurfaceMetrics metrics{logical_width, logical_height, framebuffer_width, framebuffer_height};
    metrics.scale_x = float(framebuffer_width) / float(logical_width);
    metrics.scale_y = float(framebuffer_height) / float(logical_height);
    return sanitize_surface_metrics(metrics);
}

} // namespace

// ---------------------------------------------------------------------------
// FbRect helpers
// ---------------------------------------------------------------------------

TEST_CASE("RmlUi FbRect area and empty detection")
{
    CHECK(area(FbRect{0, 0, 100, 200}) == 20000u);
    CHECK(area(FbRect{0, 0, 0, 100}) == 0u);
    CHECK(area(FbRect{0, 0, 100, 0}) == 0u);
    CHECK(area(FbRect{0, 0, -10, 100}) == 0u);
    CHECK(area(FbRect{0, 0, 100, -10}) == 0u);

    CHECK_FALSE(is_empty(FbRect{0, 0, 1, 1}));
    CHECK(is_empty(FbRect{0, 0, 0, 1}));
    CHECK(is_empty(FbRect{0, 0, 1, 0}));
    CHECK(is_empty(FbRect{0, 0, -1, 1}));
    CHECK(is_empty(FbRect{0, 0, 1, -1}));
}

TEST_CASE("RmlUi FbRect intersection")
{
    const FbRect a{10, 20, 100, 80};

    SECTION("fully overlapping")
    {
        const FbRect result = intersect(a, {10, 20, 100, 80});
        CHECK(result.x == 10);
        CHECK(result.y == 20);
        CHECK(result.w == 100);
        CHECK(result.h == 80);
    }

    SECTION("partial overlap")
    {
        const FbRect result = intersect(a, {50, 40, 100, 100});
        CHECK(result.x == 50);
        CHECK(result.y == 40);
        CHECK(result.w == 60);
        CHECK(result.h == 60);
    }

    SECTION("no overlap")
    {
        const FbRect result = intersect(a, {200, 200, 50, 50});
        CHECK(result.w == 0);
        CHECK(result.h == 0);
    }

    SECTION("edge-touching is empty")
    {
        const FbRect result = intersect(a, {110, 20, 50, 80});
        CHECK(is_empty(result));
    }

    SECTION("contained")
    {
        const FbRect result = intersect(a, {20, 30, 40, 40});
        CHECK(result.x == 20);
        CHECK(result.y == 30);
        CHECK(result.w == 40);
        CHECK(result.h == 40);
    }
}

TEST_CASE("RmlUi FbRect union")
{
    SECTION("disjoint")
    {
        const auto result = union_rects(FbRect{0, 0, 10, 10}, FbRect{20, 20, 10, 10});
        CHECK(result.x == 0);
        CHECK(result.y == 0);
        CHECK(result.w == 30);
        CHECK(result.h == 30);
    }

    SECTION("overlapping")
    {
        const auto result = union_rects(FbRect{0, 0, 20, 20}, FbRect{10, 10, 20, 20});
        CHECK(result.x == 0);
        CHECK(result.y == 0);
        CHECK(result.w == 30);
        CHECK(result.h == 30);
    }

    SECTION("with empty")
    {
        const auto result = union_rects(FbRect{5, 5, 15, 15}, FbRect{});
        CHECK(result.x == 5);
        CHECK(result.y == 5);
        CHECK(result.w == 15);
        CHECK(result.h == 15);
    }

    SECTION("both empty")
    {
        const auto result = union_rects(FbRect{}, FbRect{});
        CHECK(is_empty(result));
    }
}

TEST_CASE("RmlUi FbRect inflation")
{
    const FbRect r{10, 20, 100, 80};

    SECTION("uniform inflation")
    {
        const auto result = inflate(r, 5, 5);
        CHECK(result.x == 5);
        CHECK(result.y == 15);
        CHECK(result.w == 110);
        CHECK(result.h == 90);
    }

    SECTION("asymmetric inflation")
    {
        const auto result = inflate(r, 10, 20);
        CHECK(result.x == 0);
        CHECK(result.y == 0);
        CHECK(result.w == 120);
        CHECK(result.h == 120);
    }

    SECTION("empty remains empty")
    {
        const auto result = inflate(FbRect{}, 10, 10);
        CHECK(is_empty(result));
    }
}

TEST_CASE("RmlUi FbRect clamp to surface")
{
    const SurfaceMetrics surface{1280, 720, 1600, 900};

    SECTION("within bounds unchanged")
    {
        const auto result = clamp_to_surface({100, 100, 400, 300}, surface);
        CHECK(result.x == 100);
        CHECK(result.y == 100);
        CHECK(result.w == 400);
        CHECK(result.h == 300);
    }

    SECTION("partially offscreen right")
    {
        const auto result = clamp_to_surface({1500, 100, 200, 100}, surface);
        CHECK(result.x == 1500);
        CHECK(result.y == 100);
        CHECK(result.w == 100);
        CHECK(result.h == 100);
    }

    SECTION("partially offscreen bottom")
    {
        const auto result = clamp_to_surface({100, 850, 200, 100}, surface);
        CHECK(result.x == 100);
        CHECK(result.y == 850);
        CHECK(result.w == 200);
        CHECK(result.h == 50);
    }

    SECTION("fully offscreen")
    {
        const auto result = clamp_to_surface({2000, 2000, 100, 100}, surface);
        CHECK(is_empty(result));
    }

    SECTION("negative origin")
    {
        const auto result = clamp_to_surface({-50, -50, 200, 150}, surface);
        CHECK(result.x == 0);
        CHECK(result.y == 0);
        CHECK(result.w == 150);
        CHECK(result.h == 100);
    }
}

TEST_CASE("RmlUi align_outward_for_render_target")
{
    CHECK(align_outward_for_render_target({0, 0, 100, 200}).w == 100);
    CHECK(align_outward_for_render_target({0, 0, 100, 200}).h == 200);
    CHECK(align_outward_for_render_target({0, 0, 0, 100}).w == 0);
    CHECK(align_outward_for_render_target({0, 0, 100, 0}).w == 0);
    CHECK(align_outward_for_render_target({0, 0, -5, 100}).w == 0);
}

TEST_CASE("RmlUi rectangle intersection helpers")
{
    CHECK(intersects(FbRect{0, 0, 10, 10}, FbRect{5, 5, 10, 10}));
    CHECK_FALSE(intersects(FbRect{0, 0, 10, 10}, FbRect{20, 20, 5, 5}));

    const Rml::Rectanglei scissor = Rml::Rectanglei::FromPositionSize({5, 5}, {10, 10});
    CHECK(intersects(scissor, FbRect{0, 0, 20, 20}));
    CHECK_FALSE(intersects(scissor, FbRect{20, 20, 5, 5}));
}

// ---------------------------------------------------------------------------
// LogicalRect helpers
// ---------------------------------------------------------------------------

TEST_CASE("RmlUi LogicalRect area and empty detection")
{
    CHECK(area(LogicalRect{0.0f, 0.0f, 100.0f, 200.0f}) == Catch::Approx(20000.0f));
    CHECK(area(LogicalRect{0.0f, 0.0f, 0.0f, 100.0f}) == 0.0f);
    CHECK(area(LogicalRect{0.0f, 0.0f, 100.0f, 0.0f}) == 0.0f);

    CHECK_FALSE(is_empty(LogicalRect{0.0f, 0.0f, 1.0f, 1.0f}));
    CHECK(is_empty(LogicalRect{0.0f, 0.0f, 0.0f, 1.0f}));
    CHECK(is_empty(LogicalRect{0.0f, 0.0f, 1.0f, 0.0f}));
}

TEST_CASE("RmlUi LogicalRect intersection")
{
    const LogicalRect a{10.0f, 20.0f, 100.0f, 80.0f};

    SECTION("partial overlap")
    {
        const auto result = intersect(a, {50.0f, 40.0f, 100.0f, 100.0f});
        CHECK(result.x == 50.0f);
        CHECK(result.y == 40.0f);
        CHECK(result.w == 60.0f);
        CHECK(result.h == 60.0f);
    }

    SECTION("no overlap")
    {
        const auto result = intersect(a, {200.0f, 200.0f, 50.0f, 50.0f});
        CHECK(is_empty(result));
    }
}

TEST_CASE("RmlUi LogicalRect union and inflation")
{
    SECTION("union")
    {
        const auto la = LogicalRect{0.0f, 0.0f, 10.0f, 10.0f};
        const auto lb = LogicalRect{5.0f, 5.0f, 20.0f, 20.0f};
        const auto result = union_rects(la, lb);
        CHECK(result.x == 0.0f);
        CHECK(result.y == 0.0f);
        CHECK(result.w == 25.0f);
        CHECK(result.h == 25.0f);
    }

    SECTION("inflate")
    {
        const auto result = inflate({10.0f, 20.0f, 100.0f, 80.0f}, 5.0f, 5.0f);
        CHECK(result.x == 5.0f);
        CHECK(result.y == 15.0f);
        CHECK(result.w == 110.0f);
        CHECK(result.h == 90.0f);
    }
}

// ---------------------------------------------------------------------------
// Coordinate conversion
// ---------------------------------------------------------------------------

TEST_CASE("RmlUi logical_to_framebuffer with integer DPR")
{
    const SurfaceMetrics surface = make_surface_metrics(1280, 720, 2560, 1440);
    const auto fb = logical_to_framebuffer(LogicalRect{10.0f, 20.0f, 100.0f, 40.0f}, surface);
    CHECK(fb.x == 20);
    CHECK(fb.y == 40);
    CHECK(fb.w == 200);
    CHECK(fb.h == 80);
}

TEST_CASE("RmlUi logical_to_framebuffer with non-integer DPR 1.25")
{
    const SurfaceMetrics surface = make_surface_metrics(1280, 720, 1600, 900);
    const auto fb = logical_to_framebuffer(LogicalRect{10.0f, 20.0f, 100.0f, 40.0f}, surface);
    // left = floor(12.5) = 12
    // right = ceil(137.5) = 138
    // top = floor(25) = 25
    // bottom = ceil(75) = 75
    // w = 138 - 12 = 126
    // h = 75 - 25 = 50
    CHECK(fb.x == 12);
    CHECK(fb.y == 25);
    CHECK(fb.w == 126);
    CHECK(fb.h == 50);
}

TEST_CASE("RmlUi logical_to_framebuffer fractional origin at non-integer DPR 1.25")
{
    const SurfaceMetrics surface = make_surface_metrics(1280, 720, 1600, 900);
    const auto fb = logical_to_framebuffer(LogicalRect{10.2f, 20.2f, 100.0f, 40.0f}, surface);
    // left = floor(10.2 * 1.25) = floor(12.75) = 12
    // right = ceil(110.2 * 1.25) = ceil(137.75) = 138
    // top = floor(20.2 * 1.25) = floor(25.25) = 25
    // bottom = ceil(60.2 * 1.25) = ceil(75.25) = 76
    // w = 138 - 12 = 126 (larger than ceil(100.0 * 1.25) = 125)
    // h = 76 - 25 = 51 (larger than ceil(40.0 * 1.25) = 50)
    CHECK(fb.x == 12);
    CHECK(fb.y == 25);
    CHECK(fb.w == 126);
    CHECK(fb.h == 51);
}

TEST_CASE("RmlUi logical_to_framebuffer zero origin")
{
    const SurfaceMetrics surface = make_surface_metrics(1280, 720, 1280, 720);
    const auto fb = logical_to_framebuffer(LogicalRect{0.0f, 0.0f, 100.0f, 100.0f}, surface);
    CHECK(fb.x == 0);
    CHECK(fb.y == 0);
    CHECK(fb.w == 100);
    CHECK(fb.h == 100);
}

TEST_CASE("RmlUi logical_to_framebuffer empty/negative size")
{
    const SurfaceMetrics surface = make_surface_metrics(1280, 720, 1280, 720);
    SECTION("negative size")
    {
        const auto fb = logical_to_framebuffer(LogicalRect{10.0f, 10.0f, -5.0f, 10.0f}, surface);
        CHECK(fb.w == 0);
        CHECK(fb.h == 0);
        CHECK(is_empty(fb));
    }
    SECTION("zero size")
    {
        const auto fb = logical_to_framebuffer(LogicalRect{10.0f, 10.0f, 0.0f, 10.0f}, surface);
        CHECK(fb.w == 0);
        CHECK(fb.h == 0);
        CHECK(is_empty(fb));
    }
}

TEST_CASE("RmlUi framebuffer_to_logical round trip")
{
    const SurfaceMetrics surface = make_surface_metrics(1280, 720, 1600, 900);
    const auto logical = framebuffer_to_logical(FbRect{100, 200, 300, 150}, surface);
    CHECK(logical.x == Catch::Approx(80.0f));
    CHECK(logical.y == Catch::Approx(160.0f));
    CHECK(logical.w == Catch::Approx(240.0f));
    CHECK(logical.h == Catch::Approx(120.0f));

    // Round-trip: logical -> fb -> logical
    const auto fb = logical_to_framebuffer(logical, surface);
    CHECK(fb.x == 100);
    CHECK(fb.y == 200);
    CHECK(fb.w == 300);
    CHECK(fb.h == 150);
}

// ---------------------------------------------------------------------------
// Geometry bounds
// ---------------------------------------------------------------------------

namespace {

Rml::Vertex vertex(float x, float y)
{
    Rml::Vertex v;
    v.position = {x, y};
    return v;
}

Rml::Span<const Rml::Vertex> span_vertices(const Rml::Vector<Rml::Vertex>& vertices)
{
    return {vertices.data(), vertices.size()};
}

Rml::Span<const int> span_indices(const Rml::Vector<int>& indices)
{
    return {indices.data(), indices.size()};
}

} // namespace

TEST_CASE("RmlUi indexed geometry bounds use drawn vertices")
{
    Rml::Vector<Rml::Vertex> vertices{vertex(100.0f, 100.0f), vertex(0.0f, 0.0f),
                                      vertex(24.0f, 0.0f), vertex(24.0f, 36.0f),
                                      vertex(0.0f, 36.0f)};
    Rml::Vector<int> indices{1, 2, 3, 1, 3, 4};

    const auto bounds =
        compute_indexed_geometry_bounds(span_vertices(vertices), span_indices(indices));

    REQUIRE(bounds.status == GeometryBoundsStatus::Valid);
    CHECK(bounds.logical.x == Catch::Approx(0.0f));
    CHECK(bounds.logical.y == Catch::Approx(0.0f));
    CHECK(bounds.logical.w == Catch::Approx(24.0f));
    CHECK(bounds.logical.h == Catch::Approx(36.0f));
}

TEST_CASE("RmlUi indexed geometry bounds reject invalid input")
{
    Rml::Vector<Rml::Vertex> vertices{vertex(0.0f, 0.0f), vertex(10.0f, 0.0f)};
    Rml::Vector<int> indices{0, 1};

    SECTION("empty vertices")
    {
        Rml::Vector<Rml::Vertex> empty_vertices;
        const auto bounds =
            compute_indexed_geometry_bounds(span_vertices(empty_vertices), span_indices(indices));
        CHECK(bounds.status == GeometryBoundsStatus::EmptyGeometry);
    }

    SECTION("empty indices")
    {
        Rml::Vector<int> empty_indices;
        const auto bounds =
            compute_indexed_geometry_bounds(span_vertices(vertices), span_indices(empty_indices));
        CHECK(bounds.status == GeometryBoundsStatus::EmptyGeometry);
    }

    SECTION("bad index")
    {
        Rml::Vector<int> bad_indices{0, 2};
        const auto bounds =
            compute_indexed_geometry_bounds(span_vertices(vertices), span_indices(bad_indices));
        CHECK(bounds.status == GeometryBoundsStatus::InvalidIndex);
    }

    SECTION("non-finite vertex")
    {
        Rml::Vector<Rml::Vertex> bad_vertices{vertex(0.0f, 0.0f),
                                              vertex(std::numeric_limits<float>::infinity(), 1.0f)};
        const auto bounds =
            compute_indexed_geometry_bounds(span_vertices(bad_vertices), span_indices(indices));
        CHECK(bounds.status == GeometryBoundsStatus::NonFiniteVertex);
    }

    SECTION("zero-area geometry")
    {
        Rml::Vector<Rml::Vertex> line_vertices{vertex(0.0f, 0.0f), vertex(10.0f, 0.0f)};
        const auto bounds =
            compute_indexed_geometry_bounds(span_vertices(line_vertices), span_indices(indices));
        CHECK(bounds.status == GeometryBoundsStatus::EmptyGeometry);
    }
}

TEST_CASE("RmlUi transformed geometry bounds handle identity and translation")
{
    const SurfaceMetrics surface = make_surface_metrics(1280, 720, 1280, 720);

    const auto bounds = compute_transformed_geometry_bounds(LogicalRect{0.0f, 0.0f, 24.0f, 36.0f},
                                                            {10.0f, 20.0f}, nullptr, surface);

    REQUIRE(bounds.status == GeometryBoundsStatus::Valid);
    CHECK(bounds.logical.x == Catch::Approx(10.0f));
    CHECK(bounds.logical.y == Catch::Approx(20.0f));
    CHECK(bounds.logical.w == Catch::Approx(24.0f));
    CHECK(bounds.logical.h == Catch::Approx(36.0f));
    CHECK(bounds.framebuffer.x == 10);
    CHECK(bounds.framebuffer.y == 20);
    CHECK(bounds.framebuffer.w == 24);
    CHECK(bounds.framebuffer.h == 36);
}

TEST_CASE("RmlUi transformed geometry bounds use non-integer DPR outward rounding")
{
    const SurfaceMetrics surface = make_surface_metrics(1280, 720, 1600, 900);

    const auto bounds = compute_transformed_geometry_bounds(LogicalRect{10.2f, 20.2f, 24.0f, 36.0f},
                                                            {0.0f, 0.0f}, nullptr, surface);

    REQUIRE(bounds.status == GeometryBoundsStatus::Valid);
    CHECK(bounds.framebuffer.x == 12);
    CHECK(bounds.framebuffer.y == 25);
    CHECK(bounds.framebuffer.w == 31);
    CHECK(bounds.framebuffer.h == 46);
}

TEST_CASE("RmlUi transformed geometry bounds apply translation before transform")
{
    const SurfaceMetrics surface = make_surface_metrics(1280, 720, 1280, 720);
    const Rml::Matrix4f transform = Rml::Matrix4f::Scale(2.0f, 2.0f, 1.0f);

    const auto bounds = compute_transformed_geometry_bounds(LogicalRect{0.0f, 0.0f, 10.0f, 10.0f},
                                                            {5.0f, 7.0f}, &transform, surface);

    REQUIRE(bounds.status == GeometryBoundsStatus::Valid);
    CHECK(bounds.logical.x == Catch::Approx(10.0f));
    CHECK(bounds.logical.y == Catch::Approx(14.0f));
    CHECK(bounds.logical.w == Catch::Approx(20.0f));
    CHECK(bounds.logical.h == Catch::Approx(20.0f));
    CHECK(bounds.framebuffer.x == 10);
    CHECK(bounds.framebuffer.y == 14);
    CHECK(bounds.framebuffer.w == 20);
    CHECK(bounds.framebuffer.h == 20);
}

TEST_CASE("RmlUi transformed geometry bounds handle scaling")
{
    const SurfaceMetrics surface = make_surface_metrics(1280, 720, 1280, 720);
    const Rml::Matrix4f transform = Rml::Matrix4f::Scale(2.0f, 3.0f, 1.0f);

    const auto bounds = compute_transformed_geometry_bounds(LogicalRect{5.0f, 10.0f, 20.0f, 10.0f},
                                                            {0.0f, 0.0f}, &transform, surface);

    REQUIRE(bounds.status == GeometryBoundsStatus::Valid);
    CHECK(bounds.logical.x == Catch::Approx(10.0f));
    CHECK(bounds.logical.y == Catch::Approx(30.0f));
    CHECK(bounds.logical.w == Catch::Approx(40.0f));
    CHECK(bounds.logical.h == Catch::Approx(30.0f));
    CHECK(bounds.framebuffer.x == 10);
    CHECK(bounds.framebuffer.y == 30);
    CHECK(bounds.framebuffer.w == 40);
    CHECK(bounds.framebuffer.h == 30);
}

TEST_CASE("RmlUi transformed geometry bounds handle rotation conservatively")
{
    const SurfaceMetrics surface = make_surface_metrics(1280, 720, 1280, 720);
    constexpr float pi = 3.14159265358979323846f;
    const Rml::Matrix4f transform =
        Rml::Matrix4f::Translate(30.0f, 40.0f, 0.0f) * Rml::Matrix4f::RotateZ(pi * 0.5f);

    const auto bounds = compute_transformed_geometry_bounds(LogicalRect{0.0f, 0.0f, 10.0f, 20.0f},
                                                            {0.0f, 0.0f}, &transform, surface);

    REQUIRE(bounds.status == GeometryBoundsStatus::Valid);
    CHECK(bounds.logical.x == Catch::Approx(10.0f).margin(0.0001f));
    CHECK(bounds.logical.y == Catch::Approx(40.0f).margin(0.0001f));
    CHECK(bounds.logical.w == Catch::Approx(20.0f).margin(0.0001f));
    CHECK(bounds.logical.h == Catch::Approx(10.0f).margin(0.0001f));
    CHECK(bounds.framebuffer.x == 10);
    CHECK(bounds.framebuffer.y == 40);
    CHECK(bounds.framebuffer.w == 20);
    CHECK(bounds.framebuffer.h == 10);
}

TEST_CASE("RmlUi transformed geometry bounds handle rotation with non-integer DPR")
{
    const SurfaceMetrics surface = make_surface_metrics(1280, 720, 1600, 900);
    constexpr float pi = 3.14159265358979323846f;
    const Rml::Matrix4f transform =
        Rml::Matrix4f::Translate(30.0f, 40.0f, 0.0f) * Rml::Matrix4f::RotateZ(pi * 0.5f);

    const auto bounds = compute_transformed_geometry_bounds(LogicalRect{0.0f, 0.0f, 10.0f, 20.0f},
                                                            {0.0f, 0.0f}, &transform, surface);

    REQUIRE(bounds.status == GeometryBoundsStatus::Valid);
    CHECK(bounds.framebuffer.x == 12);
    CHECK(bounds.framebuffer.y == 50);
    CHECK(bounds.framebuffer.w == 26);
    CHECK(bounds.framebuffer.h == 13);
}

TEST_CASE("RmlUi transformed geometry bounds handle negative scale conservatively")
{
    const SurfaceMetrics surface = make_surface_metrics(1280, 720, 1280, 720);
    const Rml::Matrix4f transform =
        Rml::Matrix4f::Translate(100.0f, 0.0f, 0.0f) * Rml::Matrix4f::Scale(-1.0f, 1.0f, 1.0f);

    const auto bounds = compute_transformed_geometry_bounds(LogicalRect{10.0f, 20.0f, 20.0f, 10.0f},
                                                            {0.0f, 0.0f}, &transform, surface);

    REQUIRE(bounds.status == GeometryBoundsStatus::Valid);
    CHECK(bounds.logical.x == Catch::Approx(70.0f));
    CHECK(bounds.logical.y == Catch::Approx(20.0f));
    CHECK(bounds.logical.w == Catch::Approx(20.0f));
    CHECK(bounds.logical.h == Catch::Approx(10.0f));
    CHECK(bounds.framebuffer.x == 70);
    CHECK(bounds.framebuffer.y == 20);
    CHECK(bounds.framebuffer.w == 20);
    CHECK(bounds.framebuffer.h == 10);
}

TEST_CASE("RmlUi transformed geometry bounds clip partially offscreen framebuffer bounds")
{
    const SurfaceMetrics surface = make_surface_metrics(1280, 720, 1280, 720);

    const auto bounds = compute_transformed_geometry_bounds(
        LogicalRect{-10.0f, -5.0f, 24.0f, 36.0f}, {0.0f, 0.0f}, nullptr, surface);

    REQUIRE(bounds.status == GeometryBoundsStatus::Valid);
    CHECK(bounds.framebuffer.x == 0);
    CHECK(bounds.framebuffer.y == 0);
    CHECK(bounds.framebuffer.w == 14);
    CHECK(bounds.framebuffer.h == 31);
}

TEST_CASE("RmlUi transformed geometry bounds reject invalid transforms")
{
    const SurfaceMetrics surface = make_surface_metrics(1280, 720, 1280, 720);
    Rml::Matrix4f non_finite_transform = Rml::Matrix4f::Identity();
    non_finite_transform[0][0] = std::numeric_limits<float>::quiet_NaN();

    const auto bad_transform = compute_transformed_geometry_bounds(
        LogicalRect{0.0f, 0.0f, 24.0f, 36.0f}, {0.0f, 0.0f}, &non_finite_transform, surface);
    CHECK(bad_transform.status == GeometryBoundsStatus::NonFiniteTransform);

    const auto non_finite_translation = compute_transformed_geometry_bounds(
        LogicalRect{0.0f, 0.0f, 24.0f, 36.0f}, {std::numeric_limits<float>::quiet_NaN(), 0.0f},
        nullptr, surface);
    CHECK(non_finite_translation.status == GeometryBoundsStatus::NonFiniteTranslation);

    const auto non_finite_bounds = compute_transformed_geometry_bounds(
        LogicalRect{0.0f, std::numeric_limits<float>::quiet_NaN(), 24.0f, 36.0f}, {0.0f, 0.0f},
        nullptr, surface);
    CHECK(non_finite_bounds.status == GeometryBoundsStatus::NonFiniteBounds);
}

TEST_CASE("RmlUi transformed geometry bounds reject zero homogeneous output")
{
    const SurfaceMetrics surface = make_surface_metrics(1280, 720, 1280, 720);
    Rml::Matrix4f zero_w_transform = Rml::Matrix4f::Identity();
    zero_w_transform[3][3] = 0.0f;

    const auto bounds = compute_transformed_geometry_bounds(
        LogicalRect{0.0f, 0.0f, 24.0f, 36.0f}, {0.0f, 0.0f}, &zero_w_transform, surface);

    CHECK(bounds.status == GeometryBoundsStatus::NonFiniteOutput);
}

// ---------------------------------------------------------------------------
// UV calculation
// ---------------------------------------------------------------------------

TEST_CASE("RmlUi UV rect calculation")
{
    SECTION("full texture")
    {
        const auto uv = uv_rect_for_source_region({0, 0, 256, 256}, 256, 256);
        CHECK(uv[0] == 0.0f);
        CHECK(uv[1] == 0.0f);
        CHECK(uv[2] == 1.0f);
        CHECK(uv[3] == 1.0f);
    }

    SECTION("sub-region")
    {
        const auto uv = uv_rect_for_source_region({64, 64, 128, 128}, 256, 256);
        CHECK(uv[0] == 0.25f);
        CHECK(uv[1] == 0.25f);
        CHECK(uv[2] == 0.75f);
        CHECK(uv[3] == 0.75f);
    }

    SECTION("edge-aligned")
    {
        const auto uv = uv_rect_for_source_region({0, 0, 128, 64}, 256, 128);
        CHECK(uv[0] == 0.0f);
        CHECK(uv[1] == 0.0f);
        CHECK(uv[2] == 0.5f);
        CHECK(uv[3] == 0.5f);
    }

    SECTION("empty texture returns zeros")
    {
        const auto uv = uv_rect_for_source_region({0, 0, 100, 100}, 0, 100);
        CHECK(uv[0] == 0.0f);
        CHECK(uv[1] == 0.0f);
        CHECK(uv[2] == 0.0f);
        CHECK(uv[3] == 0.0f);
    }
}

// ---------------------------------------------------------------------------
// Filter expansion
// ---------------------------------------------------------------------------

TEST_CASE("RmlUi blur expansion is conservative")
{
    SECTION("sigma zero returns zero expansion")
    {
        const auto exp = blur_expansion(0.0f);
        CHECK(exp.left == 0);
        CHECK(exp.top == 0);
        CHECK(exp.right == 0);
        CHECK(exp.bottom == 0);
    }

    SECTION("sigma 2.0 produces ceil(6) = 6")
    {
        const auto exp = blur_expansion(2.0f);
        CHECK(exp.left == 6);
        CHECK(exp.top == 6);
        CHECK(exp.right == 6);
        CHECK(exp.bottom == 6);
    }

    SECTION("sigma 4.5 produces ceil(13.5) = 14")
    {
        const auto exp = blur_expansion(4.5f);
        CHECK(exp.left == 14);
        CHECK(exp.top == 14);
        CHECK(exp.right == 14);
        CHECK(exp.bottom == 14);
    }

    SECTION("sigma 0.1 rounds up to ceil(0.3) = 1")
    {
        const auto exp = blur_expansion(0.1f);
        CHECK(exp.left == 1);
    }
}

TEST_CASE("RmlUi drop-shadow expansion accounts for offset and blur")
{
    SECTION("offset only, no blur")
    {
        const auto exp = drop_shadow_expansion(0.0f, 5.0f, -3.0f);
        CHECK(exp.left == 0);
        CHECK(exp.top == 3);
        CHECK(exp.right == 5);
        CHECK(exp.bottom == 0);
    }

    SECTION("blur only, no offset")
    {
        const auto exp = drop_shadow_expansion(2.0f, 0.0f, 0.0f);
        CHECK(exp.left == 6);
        CHECK(exp.top == 6);
        CHECK(exp.right == 6);
        CHECK(exp.bottom == 6);
    }

    SECTION("blur + offset in both directions")
    {
        // sigma=3 => ceil(3*3)=9 blur radius applies to all sides.
        // Positive offset_x adds to right, negative offset_y adds to top.
        const auto exp = drop_shadow_expansion(3.0f, 4.0f, -5.0f);
        CHECK(exp.left == 9);   // blur only (offset_x > 0)
        CHECK(exp.top == 14);   // blur + ceil(|-5|) = 9 + 5 = 14
        CHECK(exp.right == 13); // blur + ceil(4) = 9 + 4 = 13
        CHECK(exp.bottom == 9); // blur only (offset_y < 0)
    }

    SECTION("zero sigma and zero offset")
    {
        const auto exp = drop_shadow_expansion(0.0f, 0.0f, 0.0f);
        CHECK(exp.left == 0);
        CHECK(exp.top == 0);
        CHECK(exp.right == 0);
        CHECK(exp.bottom == 0);
    }
}

TEST_CASE("RmlUi max_expansions uses per-edge max")
{
    const FilterExpansion a{1, 2, 3, 4};
    const FilterExpansion b{5, 1, 2, 6};
    const auto combined = max_expansions(a, b);
    CHECK(combined.left == 5);
    CHECK(combined.top == 2);
    CHECK(combined.right == 3);
    CHECK(combined.bottom == 6);
}

TEST_CASE("RmlUi add_expansions is additive")
{
    const FilterExpansion a{1, 2, 3, 4};
    const FilterExpansion b{5, 1, 2, 6};
    const auto added = add_expansions(a, b);
    CHECK(added.left == 6);
    CHECK(added.top == 3);
    CHECK(added.right == 5);
    CHECK(added.bottom == 10);
}

TEST_CASE("RmlUi filter_chain_expansion processes sequential chain")
{
    const std::array<FilterExpansion, 3> chain = {
        {FilterExpansion{1, 2, 3, 4}, FilterExpansion{5, 1, 2, 6}, FilterExpansion{0, 3, 1, 2}}};
    const auto total = filter_chain_expansion(chain);
    CHECK(total.left == 6);
    CHECK(total.top == 6);
    CHECK(total.right == 6);
    CHECK(total.bottom == 12);
}

TEST_CASE("RmlUi expand_bounds applies filter expansion")
{
    const FbRect r{100, 100, 200, 150};
    const FilterExpansion exp{10, 20, 30, 40};

    const auto result = expand_bounds(r, exp);
    CHECK(result.x == 90);
    CHECK(result.y == 80);
    CHECK(result.w == 240);
    CHECK(result.h == 210);

    SECTION("empty stays empty")
    {
        const auto empty_result = expand_bounds({}, exp);
        CHECK(is_empty(empty_result));
    }
}

TEST_CASE("RmlUi expand_bounds and clamp_to_surface compose for bounded work areas")
{
    const SurfaceMetrics surface{1280, 720, 1280, 720};
    const FbRect source{100, 100, 50, 40};
    const FilterExpansion exp{20, 10, 30, 15};

    const auto expanded = expand_bounds(source, exp);
    CHECK(expanded.x == 80);
    CHECK(expanded.y == 90);
    CHECK(expanded.w == 100);
    CHECK(expanded.h == 65);

    const auto clamped = clamp_to_surface(expanded, surface);
    CHECK(clamped.x == 80);
    CHECK(clamped.y == 90);
    CHECK(clamped.w == 100);
    CHECK(clamped.h == 65);
}

// ---------------------------------------------------------------------------
// Negative / offscreen source rectangles
// ---------------------------------------------------------------------------

TEST_CASE("RmlUi negative offscreen source clamps correctly")
{
    const SurfaceMetrics surface = make_surface_metrics(1280, 720, 1600, 900);

    const auto clamped = clamp_to_surface({-200, -100, 400, 300}, surface);
    CHECK(clamped.x == 0);
    CHECK(clamped.y == 0);
    CHECK(clamped.w == 200);
    CHECK(clamped.h == 200);

    SECTION("uv for clamped region")
    {
        const auto uv = uv_rect_for_source_region(clamped, 400, 300);
        CHECK(uv[0] == 0.0f);
        CHECK(uv[1] == 0.0f);
        CHECK(uv[2] == 0.5f);
        CHECK(uv[3] == Catch::Approx(0.6666667f));
    }
}

TEST_CASE("RmlUi materialized layer mapping converts between global and local coordinates")
{
    LayerRecord layer;
    layer.bounds.framebuffer = {120, 80, 300, 200};
    layer.texture_width = 300;
    layer.texture_height = 200;

    const LocalFbRect local = local_rect_for_layer(GlobalFbRect{150, 100, 50, 40}, layer);
    CHECK(local.x == 30);
    CHECK(local.y == 20);
    CHECK(local.w == 50);
    CHECK(local.h == 40);

    const GlobalFbRect global = global_rect_for_layer(local, layer);
    CHECK(global.x == 150);
    CHECK(global.y == 100);
    CHECK(global.w == 50);
    CHECK(global.h == 40);

    const LocalFbRect clipped = local_rect_for_layer(GlobalFbRect{100, 60, 60, 50}, layer);
    CHECK(clipped.x == 0);
    CHECK(clipped.y == 0);
    CHECK(clipped.w == 40);
    CHECK(clipped.h == 30);

    CHECK(is_empty(local_rect_for_layer(GlobalFbRect{0, 0, 20, 20}, layer)));

    const LocalFbRect full = full_local_rect(layer);
    CHECK(full.x == 0);
    CHECK(full.y == 0);
    CHECK(full.w == 300);
    CHECK(full.h == 200);
}

TEST_CASE("RmlUi global and target-local mapping helpers round trip bounded rectangles")
{
    const GlobalFbRect target{120, 80, 300, 200};

    const LocalFbRect interior = local_rect_for_global_rect(GlobalFbRect{150, 100, 50, 40}, target);
    CHECK(interior.x == 30);
    CHECK(interior.y == 20);
    CHECK(interior.w == 50);
    CHECK(interior.h == 40);

    const GlobalFbRect round_trip = global_rect_for_local_rect(interior, target);
    CHECK(round_trip.x == 150);
    CHECK(round_trip.y == 100);
    CHECK(round_trip.w == 50);
    CHECK(round_trip.h == 40);

    const LocalFbRect clipped = local_rect_for_global_rect(GlobalFbRect{100, 60, 60, 50}, target);
    CHECK(clipped.x == 0);
    CHECK(clipped.y == 0);
    CHECK(clipped.w == 40);
    CHECK(clipped.h == 30);

    CHECK(is_empty(local_rect_for_global_rect(GlobalFbRect{0, 0, 20, 20}, target)));

    const GlobalFbRect full_frame{0, 0, 800, 600};
    const LocalFbRect identity =
        local_rect_for_global_rect(GlobalFbRect{0, 0, 800, 600}, full_frame);
    CHECK(identity.x == 0);
    CHECK(identity.y == 0);
    CHECK(identity.w == 800);
    CHECK(identity.h == 600);
}

TEST_CASE("RmlUi texture region mapping helpers keep global and texture-local rects aligned")
{
    TextureRegion source;
    source.texture = BGFX_INVALID_HANDLE;
    source.global_bounds = {100, 50, 80, 40};
    source.local_rect = {10, 20, 80, 40};
    source.texture_width = 256;
    source.texture_height = 128;

    const LocalFbRect contained =
        texture_local_rect_for_global_rect(source, GlobalFbRect{120, 60, 40, 20});
    CHECK(contained.x == 30);
    CHECK(contained.y == 30);
    CHECK(contained.w == 40);
    CHECK(contained.h == 20);

    const LocalFbRect clipped =
        texture_local_rect_for_global_rect(source, GlobalFbRect{160, 70, 40, 40});
    CHECK(clipped.x == 70);
    CHECK(clipped.y == 40);
    CHECK(clipped.w == 20);
    CHECK(clipped.h == 20);

    CHECK(is_empty(texture_local_rect_for_global_rect(source, GlobalFbRect{0, 0, 10, 10})));

    const TextureRegion sub =
        texture_subregion_for_global_rect(source, GlobalFbRect{120, 60, 40, 20});
    CHECK(sub.global_bounds.x == 120);
    CHECK(sub.global_bounds.y == 60);
    CHECK(sub.global_bounds.w == 40);
    CHECK(sub.global_bounds.h == 20);
    CHECK(sub.local_rect.x == 30);
    CHECK(sub.local_rect.y == 30);
    CHECK(sub.local_rect.w == 40);
    CHECK(sub.local_rect.h == 20);
    CHECK(sub.texture_width == 256);
    CHECK(sub.texture_height == 128);
}

TEST_CASE("RmlUi filter-copy mapping scenario uses canonical global and local helpers")
{
    TextureRegion source;
    source.global_bounds = {100, 50, 80, 40};
    source.local_rect = {10, 20, 80, 40};
    source.texture_width = 256;
    source.texture_height = 128;

    const GlobalFbRect clamped_work_bounds{120, 60, 40, 20};
    const GlobalFbRect source_copy_global = intersect(source.global_bounds, clamped_work_bounds);
    const LocalFbRect source_copy_local =
        texture_local_rect_for_global_rect(source, source_copy_global);
    const LocalFbRect copy_destination =
        local_rect_for_global_rect(source_copy_global, clamped_work_bounds);

    CHECK(source_copy_global.x == 120);
    CHECK(source_copy_global.y == 60);
    CHECK(source_copy_global.w == 40);
    CHECK(source_copy_global.h == 20);
    CHECK(source_copy_local.x == 30);
    CHECK(source_copy_local.y == 30);
    CHECK(source_copy_local.w == 40);
    CHECK(source_copy_local.h == 20);
    CHECK(copy_destination.x == 0);
    CHECK(copy_destination.y == 0);
    CHECK(copy_destination.w == 40);
    CHECK(copy_destination.h == 20);
}

TEST_CASE(
    "RmlUi non-transformed bounded replay keeps global draw and local scissor spaces separate")
{
    LayerRecord layer;
    layer.bounds.framebuffer = {120, 80, 300, 200};
    layer.texture_width = 300;
    layer.texture_height = 200;

    const GlobalFbRect global_geometry{150, 100, 90, 60};
    const GlobalFbRect global_scissor{140, 90, 50, 40};
    const GlobalFbRect visible_global = intersect(global_geometry, global_scissor);
    const LocalFbRect visible_local = local_rect_for_layer(visible_global, layer);
    const auto local_scissor =
        clamp_scissor_local(Rml::Rectanglei::FromPositionSize({global_scissor.x, global_scissor.y},
                                                              {global_scissor.w, global_scissor.h}),
                            layer.bounds.framebuffer);

    CHECK(visible_global.x == 150);
    CHECK(visible_global.y == 100);
    CHECK(visible_global.w == 40);
    CHECK(visible_global.h == 30);
    CHECK(visible_local.x == 30);
    CHECK(visible_local.y == 20);
    CHECK(visible_local.w == 40);
    CHECK(visible_local.h == 30);
    CHECK(local_scissor.Left() == 20);
    CHECK(local_scissor.Top() == 10);
    CHECK(local_scissor.Width() == 50);
    CHECK(local_scissor.Height() == 40);
}

TEST_CASE("RmlUi bounded layer composite maps global output to destination-local rectangle")
{
    LayerRecord destination;
    destination.bounds.framebuffer = {80, 40, 200, 150};
    destination.texture_width = 200;
    destination.texture_height = 150;

    const GlobalFbRect output_global{100, 50, 80, 40};
    const LocalFbRect destination_local = local_rect_for_layer(output_global, destination);

    CHECK(destination_local.x == 20);
    CHECK(destination_local.y == 10);
    CHECK(destination_local.w == 80);
    CHECK(destination_local.h == 40);

    const GlobalFbRect clipped_output_global{250, 160, 80, 60};
    const LocalFbRect clipped_destination_local =
        local_rect_for_layer(clipped_output_global, destination);

    CHECK(clipped_destination_local.x == 170);
    CHECK(clipped_destination_local.y == 120);
    CHECK(clipped_destination_local.w == 30);
    CHECK(clipped_destination_local.h == 30);
}

TEST_CASE("RmlUi bounded texture sampling respects non-zero texture-local origins")
{
    TextureRegion source;
    source.global_bounds = {200, 120, 100, 50};
    source.local_rect = {16, 24, 100, 50};
    source.texture_width = 256;
    source.texture_height = 128;

    const GlobalFbRect sample_global{225, 130, 40, 20};
    const LocalFbRect sample_local = texture_local_rect_for_global_rect(source, sample_global);
    const TextureRegion sampled = texture_subregion_for_global_rect(source, sample_global);

    CHECK(sample_local.x == 41);
    CHECK(sample_local.y == 34);
    CHECK(sample_local.w == 40);
    CHECK(sample_local.h == 20);
    CHECK(sampled.global_bounds.x == 225);
    CHECK(sampled.global_bounds.y == 130);
    CHECK(sampled.global_bounds.w == 40);
    CHECK(sampled.global_bounds.h == 20);
    CHECK(sampled.local_rect.x == 41);
    CHECK(sampled.local_rect.y == 34);
    CHECK(sampled.local_rect.w == 40);
    CHECK(sampled.local_rect.h == 20);
    CHECK(sampled.texture_width == 256);
    CHECK(sampled.texture_height == 128);
}

TEST_CASE("RmlUi materialized layer scissor mapping clamps to target-local bounds")
{
    const GlobalFbRect layer_bounds{100, 50, 200, 150};

    const auto partial =
        clamp_scissor_local(Rml::Rectanglei::FromPositionSize({150, 80}, {200, 100}), layer_bounds);
    CHECK(partial.Left() == 50);
    CHECK(partial.Top() == 30);
    CHECK(partial.Width() == 150);
    CHECK(partial.Height() == 100);

    const auto full =
        clamp_scissor_local(Rml::Rectanglei::FromPositionSize({90, 40}, {240, 190}), layer_bounds);
    CHECK(full.Left() == 0);
    CHECK(full.Top() == 0);
    CHECK(full.Width() == 200);
    CHECK(full.Height() == 150);

    const auto none =
        clamp_scissor_local(Rml::Rectanglei::FromPositionSize({0, 0}, {20, 20}), layer_bounds);
    CHECK(none.Width() == 0);
    CHECK(none.Height() == 0);

    const auto exact =
        clamp_scissor_local(Rml::Rectanglei::FromPositionSize({100, 50}, {200, 150}), layer_bounds);
    CHECK(exact.Left() == 0);
    CHECK(exact.Top() == 0);
    CHECK(exact.Width() == 200);
    CHECK(exact.Height() == 150);

    const auto negative_origin =
        clamp_scissor_local(Rml::Rectanglei::FromPositionSize({-25, -10}, {160, 80}), layer_bounds);
    CHECK(negative_origin.Left() == 0);
    CHECK(negative_origin.Top() == 0);
    CHECK(negative_origin.Width() == 35);
    CHECK(negative_origin.Height() == 20);

    const auto right_bottom =
        clamp_scissor_local(Rml::Rectanglei::FromPositionSize({250, 160}, {100, 80}), layer_bounds);
    CHECK(right_bottom.Left() == 150);
    CHECK(right_bottom.Top() == 110);
    CHECK(right_bottom.Width() == 50);
    CHECK(right_bottom.Height() == 40);

    const auto zero_layer = clamp_scissor_local(
        Rml::Rectanglei::FromPositionSize({100, 50}, {200, 150}), GlobalFbRect{100, 50, 0, 0});
    CHECK(zero_layer.Width() == 0);
    CHECK(zero_layer.Height() == 0);
}

TEST_CASE("RmlUi compute_child_layer_bounds selection policy")
{
    const SurfaceMetrics surface = make_surface_metrics(1000, 500, 2000, 1000); // scale = 2.0

    SECTION("scissor enabled uses scissor region")
    {
        FbRect scissor = {100, 200, 300, 400};
        const auto result = compute_child_layer_bounds(surface, nullptr, &scissor, false);
        CHECK(result.framebuffer.x == 100);
        CHECK(result.framebuffer.y == 200);
        CHECK(result.framebuffer.w == 300);
        CHECK(result.framebuffer.h == 400);
        CHECK(result.logical.x == Catch::Approx(50.0f));
        CHECK(result.logical.y == Catch::Approx(100.0f));
        CHECK(result.logical.w == Catch::Approx(150.0f));
        CHECK(result.logical.h == Catch::Approx(200.0f));
    }

    SECTION("no scissor falls back to surface bounds")
    {
        const auto result = compute_child_layer_bounds(surface, nullptr, nullptr, false);
        CHECK(result.framebuffer.x == 0);
        CHECK(result.framebuffer.y == 0);
        CHECK(result.framebuffer.w == 2000);
        CHECK(result.framebuffer.h == 1000);
    }

    SECTION("transform invalidates scissor bounds and falls back to full surface")
    {
        FbRect scissor = {100, 200, 300, 400};
        const auto result = compute_child_layer_bounds(surface, nullptr, &scissor, true);
        CHECK(result.framebuffer.x == 0);
        CHECK(result.framebuffer.y == 0);
        CHECK(result.framebuffer.w == 2000);
        CHECK(result.framebuffer.h == 1000);
    }

    SECTION("clamping to parent bounds")
    {
        FbRect scissor = {100, 100, 800, 800};
        RenderBounds parent;
        parent.framebuffer = {200, 200, 400, 400};
        parent.logical = {100.0f, 100.0f, 200.0f, 200.0f};

        const auto result = compute_child_layer_bounds(surface, &parent, &scissor, false);
        // intersection of [100, 100, 800, 800] and [200, 200, 400, 400] is [200, 200, 400, 400]
        CHECK(result.framebuffer.x == 200);
        CHECK(result.framebuffer.y == 200);
        CHECK(result.framebuffer.w == 400);
        CHECK(result.framebuffer.h == 400);
    }

    SECTION("never allocate zero-size layers")
    {
        FbRect scissor = {100, 100, -50, 0};
        const auto result = compute_child_layer_bounds(surface, nullptr, &scissor, false);
        CHECK(result.framebuffer.w == 2000);
        CHECK(result.framebuffer.h == 1000);
    }
}

TEST_CASE("RmlUi mask UV transform maps global work bounds into saved mask bounds")
{
    SECTION("same-rect bounded mask samples directly")
    {
        const auto uv = compute_mask_uv_transform({476, 16, 96, 96}, {476, 16, 96, 96});
        CHECK(uv[0] == Catch::Approx(1.0f));
        CHECK(uv[1] == Catch::Approx(1.0f));
        CHECK(uv[2] == Catch::Approx(0.0f));
        CHECK(uv[3] == Catch::Approx(0.0f));
    }

    SECTION("full-frame work sampling bounded mask keeps global offset")
    {
        const auto uv = compute_mask_uv_transform({0, 0, 800, 600}, {476, 16, 96, 96});
        CHECK(uv[0] == Catch::Approx(800.0f / 96.0f));
        CHECK(uv[1] == Catch::Approx(600.0f / 96.0f));
        CHECK(uv[2] == Catch::Approx(-476.0f / 96.0f));
        CHECK(uv[3] == Catch::Approx(-16.0f / 96.0f));
    }

    SECTION("expanded work area around bounded mask accounts for expansion offset")
    {
        const auto uv = compute_mask_uv_transform({468, 8, 112, 112}, {476, 16, 96, 96});
        CHECK(uv[0] == Catch::Approx(112.0f / 96.0f));
        CHECK(uv[1] == Catch::Approx(112.0f / 96.0f));
        CHECK(uv[2] == Catch::Approx(-8.0f / 96.0f));
        CHECK(uv[3] == Catch::Approx(-8.0f / 96.0f));
    }

    SECTION("non-origin destination inside larger mask keeps positive global offset")
    {
        const auto uv = compute_mask_uv_transform({500, 40, 32, 24}, {476, 16, 96, 96});
        CHECK(uv[0] == Catch::Approx(32.0f / 96.0f));
        CHECK(uv[1] == Catch::Approx(24.0f / 96.0f));
        CHECK(uv[2] == Catch::Approx(24.0f / 96.0f));
        CHECK(uv[3] == Catch::Approx(24.0f / 96.0f));
    }

    SECTION("destination larger than mask exposes scale greater than one")
    {
        const auto uv = compute_mask_uv_transform({400, 0, 240, 144}, {476, 16, 96, 96});
        CHECK(uv[0] == Catch::Approx(240.0f / 96.0f));
        CHECK(uv[1] == Catch::Approx(144.0f / 96.0f));
        CHECK(uv[2] == Catch::Approx(-76.0f / 96.0f));
        CHECK(uv[3] == Catch::Approx(-16.0f / 96.0f));
    }
}
