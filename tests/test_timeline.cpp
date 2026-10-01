#include "ftk/test/imgui_harness.hpp"
#include "ui/timeline_widget.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;
using fjell::TimelineLane;
using fjell::TimelineMarker;
using fjell::TimelineWidget;
using ftk::test::ImGuiHarness;

namespace {

// A four-second timeline with two notifies and two emitters, stopped.
void set_up(TimelineWidget& t) {
    t.set_title("Timeline##test");
    t.set_duration(4.0f);
    t.set_playing(false);
    t.set_markers({{0.5f, "footstep"}, {2.0f, "sound"}});
    t.set_lanes({{"sparks", 0.0f, 1.0f, false}, {"smoke", 1.0f, 2.0f, false}});
}

} // namespace

TEST_CASE("Dragging on the ruler scrubs the playhead", "[ui][timeline]") {
    TimelineWidget t;
    set_up(t);
    float scrubbed = -1.0f;
    auto c = t.on_scrub.bind([&](float time) { scrubbed = time; });
    ImGuiHarness h;
    h.set_ui([&] { t.draw(0.0f); });
    h.step(3);

    h.drag(t.screen_position(1.0f), t.screen_position(3.0f));
    CHECK(t.time() == Approx(3.0f).margin(0.05));
    CHECK(scrubbed == Approx(t.time()));
}

TEST_CASE("A marker drags along the ruler", "[ui][timeline]") {
    TimelineWidget t;
    set_up(t);
    int moved = -1;
    float moved_to = 0.0f;
    auto c = t.on_marker_moved.bind([&](int i, float time) {
        moved = i;
        moved_to = time;
    });
    ImGuiHarness h;
    h.set_ui([&] { t.draw(0.0f); });
    h.step(3);

    h.drag(t.screen_position(2.0f), t.screen_position(3.5f));
    CHECK(moved == 1);
    CHECK(moved_to == Approx(3.5f).margin(0.05));
    CHECK(t.selected_marker() == 1);
    CHECK(t.time() == 0.0f);   // taking a marker is not a scrub
}

TEST_CASE("A lane drags by where it was taken, and stays inside the timeline", "[ui][timeline]") {
    TimelineWidget t;
    set_up(t);
    int selected = -1;
    float start = -1.0f;
    auto a = t.on_lane_selected.bind([&](int i) { selected = i; });
    auto b = t.on_lane_dragged.bind([&](int, float s) { start = s; });
    ImGuiHarness h;
    h.set_ui([&] { t.draw(0.0f); });
    h.step(3);

    // Taken a second into the smoke (which starts at 1), moved half a second on.
    h.drag(t.screen_position(2.0f, 1), t.screen_position(2.5f, 1));
    CHECK(selected == 1);
    CHECK(start == Approx(1.5f).margin(0.05));

    // Pushed far past the end: it stops where it ends with the timeline.
    h.drag(t.screen_position(2.5f, 1), t.screen_position(9.0f, 1));
    CHECK(start == Approx(2.0f).margin(0.05));
}

TEST_CASE("Right-clicking the ruler says when", "[ui][timeline]") {
    TimelineWidget t;
    set_up(t);
    float at = -1.0f;
    auto c = t.on_right_click.bind([&](float time) { at = time; });
    ImGuiHarness h;
    h.set_ui([&] { t.draw(0.0f); });
    h.step(3);

    const ImVec2 p = t.screen_position(1.25f);
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(p.x, p.y);
    h.step();
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
    h.step();
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
    h.step();
    CHECK(at == Approx(1.25f).margin(0.05));
}

TEST_CASE("A timeline showing all of itself goes on doing so when it grows", "[ui][timeline]") {
    TimelineWidget t;
    set_up(t);
    ImGuiHarness h;
    h.set_ui([&] { t.draw(0.0f); });
    h.step(3);
    const float end = t.screen_position(4.0f).x;

    t.set_duration(8.0f);
    h.step();
    CHECK(t.screen_position(8.0f).x == Approx(end));
}
