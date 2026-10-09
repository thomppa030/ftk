#include "kit_harness.hpp"

#include "ftk/ui/kit/gradient_editor.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

using Catch::Approx;
using ftk::ColorGradient;
using ftk::test::KitHarness;
namespace ui = ftk::ui;

namespace {

bool close(const glm::vec3& a, const glm::vec3& b) {
    return glm::length(a - b) < 1e-3f;
}

ColorGradient three_stops() {
    ColorGradient g;
    g.keys = {{0.0f, glm::vec3(0.0f)}, {0.5f, glm::vec3(1.0f, 0.0f, 0.0f)}, {1.0f, glm::vec3(1.0f)}};
    return g;
}

// One editor, and where its bar and stops are on screen as last drawn.
struct Editor {
    ColorGradient gradient = three_stops();
    ui::Edit last{};
    bool committed = false;
    glm::vec2 bar_start{};
    glm::vec2 bar_end{};
    glm::vec2 stop_start{};
    glm::vec2 stop_end{};

    void draw() {
        last = ui::gradient_editor("##ramp", gradient);
        committed |= last.committed;
        bar_start = ui::gradient_editor_bar_point("##ramp", 0.0f);
        bar_end = ui::gradient_editor_bar_point("##ramp", 1.0f);
        stop_start = ui::gradient_editor_stop_point("##ramp", 0.0f);
        stop_end = ui::gradient_editor_stop_point("##ramp", 1.0f);
    }
    [[nodiscard]] glm::vec2 stop(float t) const { return glm::mix(stop_start, stop_end, t); }
    [[nodiscard]] glm::vec2 bar(float t) const { return glm::mix(bar_start, bar_end, t); }
};

} // namespace

TEST_CASE("A click on the gradient bar adds a stop in the colour already there", "[ui][gradient_editor]") {
    KitHarness h;
    Editor e;
    h.set_ui([&] { e.draw(); });
    h.step(2);

    const glm::vec3 before = e.gradient.evaluate(0.25f);
    h.click_at(e.bar(0.25f));
    REQUIRE(e.gradient.keys.size() == 4);
    CHECK(e.gradient.keys[1].time == Approx(0.25f).margin(0.01f));
    CHECK(close(e.gradient.keys[1].color, before));
    CHECK(e.committed);
    // The colour across the bar is what it was.
    CHECK(close(e.gradient.evaluate(0.25f), before));
}

TEST_CASE("A stop dragged moves between its neighbours and commits once let go", "[ui][gradient_editor]") {
    KitHarness h;
    Editor e;
    h.set_ui([&] { e.draw(); });
    h.step(2);

    h.drag(e.stop(0.5f), e.stop(0.7f));
    CHECK(e.gradient.keys[1].time == Approx(0.7f).margin(0.01f));
    CHECK(e.committed);

    // Dragged past the last stop, it stops at it: the stops keep their order.
    h.drag(e.stop(0.7f), e.stop(1.0f) + glm::vec2{60.0f, 0.0f});
    CHECK(e.gradient.keys[1].time == Approx(1.0f));
    CHECK(e.gradient.keys[0].time <= e.gradient.keys[1].time);
}

TEST_CASE("Shift moves a dragged stop a tenth as far", "[ui][gradient_editor]") {
    KitHarness h;
    Editor e;
    h.set_ui([&] { e.draw(); });
    h.step(2);

    h.hold({.shift = true});
    h.drag(e.stop(0.5f), e.stop(0.7f));
    h.hold({});
    CHECK(e.gradient.keys[1].time == Approx(0.52f).margin(0.005f));
}

TEST_CASE("A stop pulled off the bar is removed, but never the last", "[ui][gradient_editor]") {
    KitHarness h;
    Editor e;
    h.set_ui([&] { e.draw(); });
    h.step(2);

    h.drag(e.stop(0.5f), e.stop(0.5f) + glm::vec2{0.0f, 120.0f});
    REQUIRE(e.gradient.keys.size() == 2);
    CHECK(e.committed);

    e.gradient.keys = {{0.3f, glm::vec3(0.5f)}};
    h.step(2);
    h.drag(e.stop(0.3f), e.stop(0.3f) + glm::vec2{0.0f, 120.0f});
    CHECK(e.gradient.keys.size() == 1);
}

TEST_CASE("Del removes the selected stop", "[ui][gradient_editor]") {
    KitHarness h;
    Editor e;
    h.set_ui([&] { e.draw(); });
    h.step(2);

    h.click_at(e.stop(0.5f));
    h.press(ui::Key::Delete);
    h.step();
    REQUIRE(e.gradient.keys.size() == 2);
    CHECK(close(e.gradient.keys[1].color, glm::vec3(1.0f)));
}
