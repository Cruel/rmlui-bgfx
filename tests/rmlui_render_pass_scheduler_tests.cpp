#include "rmlui_bgfx_pass_scheduler.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace rmlui_bgfx;

static RmlUiPassRequest request(RmlUiPassKind kind, uintptr_t framebuffer, bool clears_color,
                                bool clears_stencil, const char* name,
                                RmlUiPassReason reason = RmlUiPassReason::Other)
{
    return {kind, framebuffer, 0, clears_color, clears_stencil, 0, 0, 800, 600, name, reason};
}

TEST_CASE("RmlUi pass scheduler reuses ordinary geometry view")
{
    RmlUiRenderPassScheduler scheduler(32, 34);
    const auto first = scheduler.acquire(request(RmlUiPassKind::Geometry, 7, false, false, "geo"));
    const auto second = scheduler.acquire(request(RmlUiPassKind::Geometry, 7, false, false, "geo"));

    REQUIRE(first);
    REQUIRE(second);
    CHECK(first->view == second->view);
    CHECK(scheduler.passes().size() == 1);
}

TEST_CASE("RmlUi pass scheduler reuses adjacent non-clear views with matching state")
{
    RmlUiRenderPassScheduler scheduler(32, 34);
    const auto first =
        scheduler.acquire(request(RmlUiPassKind::LayerComposite, 7, false, false, "comp"));
    const auto second =
        scheduler.acquire(request(RmlUiPassKind::LayerComposite, 7, false, false, "comp"));

    REQUIRE(first);
    REQUIRE(second);
    CHECK(first->view == second->view);
    CHECK(second->reused);
    CHECK(scheduler.passes().size() == 1);
}

TEST_CASE("RmlUi pass scheduler merges geometry-like draw into preceding clear view")
{
    RmlUiRenderPassScheduler scheduler(32, 36);
    const auto composite =
        scheduler.acquire(request(RmlUiPassKind::LayerComposite, 7, false, false, "comp"));
    const auto clear = scheduler.acquire(
        request(RmlUiPassKind::Clear, 7, true, true, "clear", RmlUiPassReason::LayerClear));
    const auto after_clear = scheduler.acquire(request(RmlUiPassKind::Geometry, 7, false, false,
                                                       "geo", RmlUiPassReason::OrdinaryGeometry));
    const auto gradient = scheduler.acquire(
        request(RmlUiPassKind::Geometry, 7, false, false, "grad", RmlUiPassReason::Gradient));

    REQUIRE(composite);
    REQUIRE(clear);
    REQUIRE(after_clear);
    REQUIRE(gradient);
    CHECK(composite->view == 32);
    CHECK(clear->view == 33);
    CHECK(after_clear->view == clear->view);
    CHECK(after_clear->reused);
    CHECK(gradient->view == clear->view);
    CHECK(gradient->reused);
}

TEST_CASE("RmlUi pass scheduler does not merge fullscreen composite into preceding clear view")
{
    RmlUiRenderPassScheduler scheduler(32, 36);
    const auto clear = scheduler.acquire(
        request(RmlUiPassKind::Clear, 7, true, true, "clear", RmlUiPassReason::LayerClear));
    const auto composite = scheduler.acquire(request(RmlUiPassKind::LayerComposite, 7, false, false,
                                                     "comp", RmlUiPassReason::LayerComposite));

    REQUIRE(clear);
    REQUIRE(composite);
    CHECK(clear->view == 32);
    CHECK(composite->view == 33);
    CHECK_FALSE(composite->reused);
}

TEST_CASE("RmlUi pass scheduler creates passes for framebuffer boundaries")
{
    RmlUiRenderPassScheduler scheduler(32, 36);
    const auto geometry =
        scheduler.acquire(request(RmlUiPassKind::Geometry, 7, false, false, "geo"));
    const auto clear = scheduler.acquire(request(RmlUiPassKind::Clear, 7, true, true, "clear"));
    const auto other_framebuffer =
        scheduler.acquire(request(RmlUiPassKind::Geometry, 8, false, false, "geo"));
    const auto postprocess =
        scheduler.acquire(request(RmlUiPassKind::Postprocess, 8, false, false, "post"));

    REQUIRE(geometry);
    REQUIRE(clear);
    REQUIRE(other_framebuffer);
    REQUIRE(postprocess);
    CHECK(geometry->view == 32);
    CHECK(clear->view == 33);
    CHECK(clear->reused == false);
    CHECK(other_framebuffer->view == 34);
    CHECK(postprocess->view == other_framebuffer->view);
    CHECK(postprocess->reused);
}

TEST_CASE("RmlUi pass scheduler reports exhaustion without reusing final view")
{
    RmlUiRenderPassScheduler scheduler(32, 33);
    REQUIRE(scheduler.acquire(request(RmlUiPassKind::Clear, 0, true, false, "a")));
    REQUIRE(scheduler.acquire(request(RmlUiPassKind::Resolve, 1, false, false, "b")));
    const auto exhausted =
        scheduler.acquire(request(RmlUiPassKind::FinalComposite, 2, false, false, "c"));

    CHECK_FALSE(exhausted);
    CHECK(scheduler.exhausted());
    CHECK(scheduler.passes().size() == 2);
}
