#include "rmlui_bgfx_bounds.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace rmlui_bgfx;

TEST_CASE("RmlUi draw bounds combine scissor and mask constraints")
{
    const FbRect draw{10, 20, 100, 80};
    const FbRect scissor{40, 0, 60, 200};
    const ConservativeMaskBounds mask{{0, 30, 200, 40}, true, false};

    const FbRect constrained = apply_mask_constraints(draw, &scissor, &mask);
    CHECK(constrained.x == 40);
    CHECK(constrained.y == 30);
    CHECK(constrained.w == 60);
    CHECK(constrained.h == 40);
}

TEST_CASE("RmlUi mask Set and Intersect produce conservative bounds")
{
    ConservativeMaskBounds state;

    state = update_conservative_mask_bounds(state, Rml::ClipMaskOperation::Set,
                                            FbRect{10, 20, 100, 80}, FbRect{});
    CHECK(state.active);
    CHECK_FALSE(state.inverse_fallback);
    CHECK(state.bounds.x == 10);
    CHECK(state.bounds.y == 20);
    CHECK(state.bounds.w == 100);
    CHECK(state.bounds.h == 80);

    state = update_conservative_mask_bounds(state, Rml::ClipMaskOperation::Intersect,
                                            FbRect{50, 10, 100, 50}, FbRect{});
    CHECK(state.active);
    CHECK_FALSE(state.inverse_fallback);
    CHECK(state.bounds.x == 50);
    CHECK(state.bounds.y == 20);
    CHECK(state.bounds.w == 60);
    CHECK(state.bounds.h == 40);
}

TEST_CASE("RmlUi inverse masks use the supplied fallback container")
{
    ConservativeMaskBounds state;
    state = update_conservative_mask_bounds(state, Rml::ClipMaskOperation::SetInverse,
                                            FbRect{20, 20, 30, 30}, FbRect{5, 10, 200, 100});

    CHECK(state.active);
    CHECK(state.inverse_fallback);
    CHECK(state.bounds.x == 5);
    CHECK(state.bounds.y == 10);
    CHECK(state.bounds.w == 200);
    CHECK(state.bounds.h == 100);

    const FbRect draw{0, 0, 500, 500};
    const FbRect constrained = apply_mask_constraints(draw, nullptr, &state);
    CHECK(constrained.x == 5);
    CHECK(constrained.y == 10);
    CHECK(constrained.w == 200);
    CHECK(constrained.h == 100);
}

TEST_CASE("RmlUi inverse masks without fallback container stay inactive")
{
    ConservativeMaskBounds state;
    state = update_conservative_mask_bounds(state, Rml::ClipMaskOperation::SetInverse,
                                            FbRect{20, 20, 30, 30}, FbRect{});

    CHECK_FALSE(state.active);
    CHECK_FALSE(state.inverse_fallback);
}
