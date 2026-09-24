#include "imgui_harness.hpp"
#include "ui/kit/canvas.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;
using fjell::test::ImGuiHarness;
namespace ui = fjell::ui;

namespace {

constexpr ImVec2 ORIGIN{100.0f, 100.0f};
constexpr ImVec2 SIZE{400.0f, 300.0f};

// A canvas at ORIGIN taking the mouse over all of it.
void draw_canvas(ui::CanvasView& view, ImVec2 size = SIZE) {
    view.place(ORIGIN, size);
    ImGui::SetCursorScreenPos(ORIGIN);
    ImGui::InvisibleButton("##canvas", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    view.input(ImGui::IsItemHovered());
}

} // namespace

TEST_CASE("The wheel zooms at the cursor", "[ui][canvas]") {
    ui::CanvasView view;
    ImGuiHarness h;
    h.set_ui([&] { draw_canvas(view); });
    h.step(2);

    const ImVec2 at{250.0f, 180.0f};
    const glm::vec2 under = view.to_world(at);
    h.wheel(at, 2.0f);

    CHECK(view.scale_x() > 1.2f);
    const glm::vec2 after = view.to_world(at);
    CHECK(after.x == Approx(under.x).margin(1e-3));
    CHECK(after.y == Approx(under.y).margin(1e-3));
}

TEST_CASE("Zoom stays inside its limits", "[ui][canvas]") {
    ui::CanvasView view({.min_scale = 0.5f, .max_scale = 2.0f});
    ImGuiHarness h;
    h.set_ui([&] { draw_canvas(view); });
    h.step(2);

    h.wheel({300.0f, 250.0f}, 40.0f);
    CHECK(view.scale_x() == Approx(2.0f));
    h.wheel({300.0f, 250.0f}, -80.0f);
    CHECK(view.scale_x() == Approx(0.5f));
}

TEST_CASE("A middle drag pans by the distance the mouse moved", "[ui][canvas]") {
    ui::CanvasView view;
    ImGuiHarness h;
    h.set_ui([&] { draw_canvas(view); });
    h.step(2);

    const glm::vec2 before = view.to_world(ORIGIN);
    h.drag({300.0f, 250.0f}, {340.0f, 230.0f}, ImGuiMouseButton_Middle);

    const glm::vec2 after = view.to_world(ORIGIN);
    CHECK(after.x == Approx(before.x - 40.0f));
    CHECK(after.y == Approx(before.y + 20.0f));
    CHECK_FALSE(view.panning());
}

TEST_CASE("Alt and a left drag pan, and the click is not the canvas's", "[ui][canvas]") {
    ui::CanvasView view;
    bool clicked = false;
    ImGuiHarness h;
    h.set_ui([&] {
        draw_canvas(view);
        if (ImGui::IsItemActivated() && !view.panning()) clicked = true;
    });
    h.step(2);

    const glm::vec2 before = view.to_world(ORIGIN);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, true);
    h.drag({300.0f, 250.0f}, {260.0f, 250.0f});
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, false);
    h.step();

    CHECK(view.to_world(ORIGIN).x == Approx(before.x + 40.0f));
    CHECK_FALSE(clicked);
}

TEST_CASE("Framing fits the world without enlarging it", "[ui][canvas]") {
    ui::CanvasView view;
    ImGuiHarness h;
    h.set_ui([&] { draw_canvas(view); });
    h.step(2);

    // Twice as wide as the canvas: half the scale, centred.
    view.frame({0.0f, 0.0f}, {800.0f, 100.0f});
    CHECK(view.scale_x() == Approx(0.5f));
    CHECK(view.to_screen({400.0f, 50.0f}).x == Approx(ORIGIN.x + SIZE.x * 0.5f));
    CHECK(view.to_screen({400.0f, 50.0f}).y == Approx(ORIGIN.y + SIZE.y * 0.5f));

    // Something small is shown at its own size, not blown up.
    view.frame({0.0f, 0.0f}, {10.0f, 10.0f});
    CHECK(view.scale_x() == Approx(1.0f));
}

TEST_CASE("A graph keeps its scale when resized; a timeline keeps its span", "[ui][canvas]") {
    ui::CanvasView graph;
    ui::CanvasView timeline({.zoom = ui::CanvasZoom::Horizontal, .bound_min = 0.0f, .bound_max = 1.0f});
    ImVec2 size = SIZE;
    ImGuiHarness h;
    h.set_ui([&] {
        graph.place(ORIGIN, size);
        timeline.place(ORIGIN, size);
    });
    h.step();
    timeline.frame({0.0f, 0.0f}, {1.0f, 0.0f});
    size = {800.0f, 300.0f};
    h.step();

    CHECK(graph.scale_x() == Approx(1.0f));
    CHECK(timeline.to_world_x(ORIGIN.x + 800.0f) == Approx(1.0f));
}

TEST_CASE("A timeline zooms and pans only inside its bounds", "[ui][canvas]") {
    ui::CanvasView view({.zoom = ui::CanvasZoom::Horizontal, .bound_min = 0.0f, .bound_max = 1.0f,
                         .min_span = 0.1f});
    ImGuiHarness h;
    h.set_ui([&] { draw_canvas(view); });
    h.step(2);
    view.frame({0.0f, 0.0f}, {1.0f, 0.0f});

    // Zooming out past everything shows everything.
    h.wheel({300.0f, 250.0f}, -10.0f);
    CHECK(view.to_world_x(ORIGIN.x) == Approx(0.0f));
    CHECK(view.to_world_x(ORIGIN.x + SIZE.x) == Approx(1.0f));

    // Zooming in stops at the least span.
    h.wheel({300.0f, 250.0f}, 60.0f);
    CHECK(view.to_world_x(ORIGIN.x + SIZE.x) - view.to_world_x(ORIGIN.x) == Approx(0.1f));

    // Panning far right stops at the end.
    h.drag({150.0f, 250.0f}, {-2000.0f, 250.0f}, ImGuiMouseButton_Middle);
    CHECK(view.to_world_x(ORIGIN.x + SIZE.x) == Approx(1.0f));
}

TEST_CASE("A canvas's view is kept per asset for the session", "[ui][canvas]") {
    ImGuiHarness h;
    {
        ui::CanvasView view;
        h.set_ui([&] { draw_canvas(view); });
        CHECK_FALSE(view.recall("test_canvas|a.fjanimset"));
        h.step(2);
        h.wheel({300.0f, 250.0f}, 3.0f);
        h.set_ui({});
    }
    ui::CanvasView reopened;
    CHECK(reopened.recall("test_canvas|a.fjanimset"));
    CHECK(reopened.scale_x() > 1.4f);
    CHECK_FALSE(reopened.recall("test_canvas|b.fjanimset"));
}
