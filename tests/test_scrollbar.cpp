#include "kit_harness.hpp"

#include "ftk/ui/kit/scrollbar.hpp"
#include "ftk/ui/theme.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;
using ftk::test::KitHarness;
namespace ui = ftk::ui;
namespace theme = ftk::theme;

namespace {

// A scrollbar 400 tall down x 300 to 310, for a view 400 tall with 1200
// more below it: the thumb is a quarter of the track.
struct Bar {
    KitHarness h;
    float offset{0.0f};
    int moves{0};

    Bar() {
        h.set_ui([this] {
            const ui::ScrollMark marks[] = {{.at = 800.0f, .colour = theme::error()}};
            if (ui::vertical_scrollbar("##bar", {300.0f, 100.0f}, {310.0f, 500.0f}, offset, 400.0f, 1200.0f, marks)) {
                ++moves;
            }
        });
        h.step(2);
    }
};

std::size_t drawn_in(const ftk::test::UiHarness& h, ui::Color colour) {
    const uint32_t packed = ui::core::pack_colour(colour);
    std::size_t count = 0;
    for (const ui::core::RootOutput& root : h.output().roots) {
        for (const ui::core::Primitive& p : root.draw->primitives) count += p.colour == packed ? 1 : 0;
    }
    return count;
}

} // namespace

TEST_CASE("Dragging a scrollbar's thumb scrolls by as much as it moves along the track", "[ui][kit][scrollbar]") {
    Bar bar;
    // The thumb is the track's top 100: 300 of travel for 1200 of room.
    bar.h.drag({305.0f, 150.0f}, {305.0f, 225.0f});
    CHECK(bar.offset == Approx(300.0f));
    CHECK(bar.moves > 0);
}

TEST_CASE("A press on a scrollbar's track takes the thumb there", "[ui][kit][scrollbar]") {
    Bar bar;
    // The thumb's middle to 350 down the track's 400: as far as it goes.
    bar.h.click_at({305.0f, 450.0f});
    CHECK(bar.offset == Approx(1200.0f));
    bar.h.click_at({305.0f, 250.0f});
    CHECK(bar.offset == Approx(400.0f));
}

TEST_CASE("A scrollbar marks where things are, and shows nothing with no room", "[ui][kit][scrollbar]") {
    Bar bar;
    bar.h.step();
    CHECK(drawn_in(bar.h, theme::error()) == 1);

    KitHarness h;
    float offset = 0.0f;
    bool moved = false;
    h.set_ui([&] { moved = ui::vertical_scrollbar("##bar", {300.0f, 100.0f}, {310.0f, 500.0f}, offset, 400.0f, 0.0f); });
    h.step(2);
    h.click_at({305.0f, 300.0f});
    CHECK_FALSE(moved);
    CHECK(offset == 0.0f);
}
