#include "kit_harness.hpp"

#include "ftk/ui/core/draw_list.hpp"
#include "ftk/ui/kit/canvas.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/item.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/plot.hpp"
#include "ftk/ui/theme.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <tuple>

using Catch::Approx;
using ftk::test::KitHarness;
namespace ui = ftk::ui;

namespace {

constexpr glm::vec2 ORIGIN{100.0f, 100.0f};
constexpr glm::vec2 SIZE{400.0f, 300.0f};

// A canvas at ORIGIN taking the mouse over all of it.
void draw_canvas(ui::CanvasView& view, glm::vec2 size = SIZE) {
    view.place(ORIGIN, size);
    ui::set_cursor(ORIGIN);
    (void)ui::hit_area("##canvas", size, {.left = true, .middle = true});
    view.input(ui::item_hovered());
}

} // namespace

TEST_CASE("The wheel zooms at the cursor", "[ui][canvas]") {
    ui::CanvasView view;
    KitHarness h;
    h.set_ui([&] { draw_canvas(view); });
    h.step(2);

    const glm::vec2 at{250.0f, 180.0f};
    const glm::vec2 under = view.to_world(at);
    h.wheel(at, 2.0f);

    CHECK(view.scale_x() > 1.2f);
    const glm::vec2 after = view.to_world(at);
    CHECK(after.x == Approx(under.x).margin(1e-3));
    CHECK(after.y == Approx(under.y).margin(1e-3));
}

TEST_CASE("A zoom of whole steps about a point keeps that point's world under it", "[ui][canvas]") {
    ui::CanvasView view;
    KitHarness h;
    h.set_ui([&] { draw_canvas(view); });
    h.step(2);

    const glm::vec2 centre = ORIGIN + SIZE * 0.5f;
    const glm::vec2 under = view.to_world(centre);
    view.zoom(1.0f, centre);
    const float one_step = view.scale_x();
    CHECK(one_step > 1.0f);
    CHECK(view.to_world(centre).x == Approx(under.x).margin(1e-3));
    CHECK(view.to_world(centre).y == Approx(under.y).margin(1e-3));

    // A step back out is where it started.
    view.zoom(-1.0f, centre);
    CHECK(view.scale_x() == Approx(1.0f));
    CHECK(view.to_world(centre).x == Approx(under.x).margin(1e-3));
}

TEST_CASE("Zoom stays inside its limits", "[ui][canvas]") {
    ui::CanvasView view({.min_scale = 0.5f, .max_scale = 2.0f});
    KitHarness h;
    h.set_ui([&] { draw_canvas(view); });
    h.step(2);

    h.wheel({300.0f, 250.0f}, 40.0f);
    CHECK(view.scale_x() == Approx(2.0f));
    h.wheel({300.0f, 250.0f}, -80.0f);
    CHECK(view.scale_x() == Approx(0.5f));
}

TEST_CASE("A middle drag pans by the distance the mouse moved", "[ui][canvas]") {
    ui::CanvasView view;
    KitHarness h;
    h.set_ui([&] { draw_canvas(view); });
    h.step(2);

    const glm::vec2 before = view.to_world(ORIGIN);
    h.drag({300.0f, 250.0f}, {340.0f, 230.0f}, ui::MouseButton::Middle);

    const glm::vec2 after = view.to_world(ORIGIN);
    CHECK(after.x == Approx(before.x - 40.0f));
    CHECK(after.y == Approx(before.y + 20.0f));
    CHECK_FALSE(view.panning());
}

TEST_CASE("Alt and a left drag pan, and the click is not the canvas's", "[ui][canvas]") {
    ui::CanvasView view;
    bool clicked = false;
    KitHarness h;
    h.set_ui([&] {
        draw_canvas(view);
        if (ui::item_activated() && !view.panning()) clicked = true;
    });
    h.step(2);

    const glm::vec2 before = view.to_world(ORIGIN);
    h.hold({.alt = true});
    h.drag({300.0f, 250.0f}, {260.0f, 250.0f});
    h.hold({});

    CHECK(view.to_world(ORIGIN).x == Approx(before.x + 40.0f));
    CHECK_FALSE(clicked);
}

TEST_CASE("Framing fits the world without enlarging it", "[ui][canvas]") {
    ui::CanvasView view;
    KitHarness h;
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
    glm::vec2 size = SIZE;
    KitHarness h;
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
    KitHarness h;
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
    h.drag({150.0f, 250.0f}, {-2000.0f, 250.0f}, ui::MouseButton::Middle);
    CHECK(view.to_world_x(ORIGIN.x + SIZE.x) == Approx(1.0f));
}

TEST_CASE("A canvas's view is kept per asset for the session", "[ui][canvas]") {
    KitHarness h;
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

TEST_CASE("A plot shows exactly what it frames, values growing upward", "[ui][canvas]") {
    ui::CanvasView view({.zoom = ui::CanvasZoom::Both, .y_up = true});
    KitHarness h;
    h.set_ui([&] { draw_canvas(view); });
    h.step(2);
    view.frame({-1.0f, -2.0f}, {3.0f, 4.0f});

    // The bottom-left corner is the lowest value, the top-right the highest.
    CHECK(view.to_world({ORIGIN.x, ORIGIN.y + SIZE.y}).x == Approx(-1.0f));
    CHECK(view.to_world({ORIGIN.x, ORIGIN.y + SIZE.y}).y == Approx(-2.0f));
    CHECK(view.to_world({ORIGIN.x + SIZE.x, ORIGIN.y}).x == Approx(3.0f));
    CHECK(view.to_world({ORIGIN.x + SIZE.x, ORIGIN.y}).y == Approx(4.0f));
    CHECK(view.to_screen({3.0f, 4.0f}).y == Approx(ORIGIN.y));
    CHECK(view.to_screen({-1.0f, -2.0f}).y == Approx(ORIGIN.y + SIZE.y));
}

TEST_CASE("A plot zooms both axes at the cursor, and across only with Shift", "[ui][canvas]") {
    ui::CanvasView view({.zoom = ui::CanvasZoom::Both, .y_up = true});
    KitHarness h;
    h.set_ui([&] { draw_canvas(view); });
    h.step(2);
    view.frame({0.0f, 0.0f}, {4.0f, 3.0f});

    const glm::vec2 at{220.0f, 310.0f};
    const glm::vec2 under = view.to_world(at);
    h.wheel(at, 2.0f);
    CHECK(view.to_world(at).x == Approx(under.x).margin(1e-4));
    CHECK(view.to_world(at).y == Approx(under.y).margin(1e-4));
    const float scale_y = view.scale_y();
    CHECK(scale_y > 100.0f * 1.2f);

    h.hold({.shift = true});
    h.wheel(at, 2.0f);
    h.hold({});
    CHECK(view.scale_y() == Approx(scale_y));
    CHECK(view.scale_x() > 100.0f * 1.5f);
}

TEST_CASE("A plot pans with the mouse and keeps its range when resized", "[ui][canvas]") {
    ui::CanvasView view({.zoom = ui::CanvasZoom::Both, .y_up = true});
    glm::vec2 size = SIZE;
    KitHarness h;
    h.set_ui([&] { draw_canvas(view, size); });
    h.step(2);
    view.frame({0.0f, 0.0f}, {4.0f, 3.0f});

    // Dragged up by a quarter of the height: what was at the centre is now
    // a quarter higher on screen, so the view shows lower values.
    const glm::vec2 before = view.to_world({300.0f, 250.0f});
    h.drag({300.0f, 250.0f}, {300.0f, 175.0f}, ui::MouseButton::Middle);
    CHECK(view.to_world({300.0f, 250.0f}).y == Approx(before.y - 0.75f));

    size = {800.0f, 600.0f};
    h.step();
    CHECK(view.to_world({ORIGIN.x + 800.0f, ORIGIN.y}).x == Approx(4.0f));
}

TEST_CASE("A plot is an item the size it is given", "[ui][kit]") {
    KitHarness h;
    const std::array<float, 4> history{1.0f, 3.0f, 2.0f, 5.0f};
    glm::vec2 size{};
    h.set_ui([&] {
        ui::plot_lines("##frame", history, 1, {200.0f, 35.0f}, {.max = 33.3f});
        size = ui::item_max() - ui::item_min();
    });
    h.step(2);
    CHECK(size == glm::vec2{200.0f, 35.0f});
}

TEST_CASE("A plot leaves room for its values and names outside it", "[ui][canvas]") {
    glm::vec2 origin{};
    glm::vec2 size{};
    ui::plot_area(ORIGIN, SIZE, {.x_name = "speed", .y_name = "direction"}, &origin, &size);
    CHECK(origin.x > ORIGIN.x + 40.0f);   // the y values and the name beside them
    CHECK(origin.y == ORIGIN.y);
    CHECK(origin.x + size.x == Approx(ORIGIN.x + SIZE.x));
    CHECK(origin.y + size.y < ORIGIN.y + SIZE.y - 16.0f);   // a row of x values under it

    // One value along a line: no y values, so nothing on the left.
    ui::plot_area(ORIGIN, SIZE, {.x_name = "speed", .y_axis = false}, &origin, &size);
    CHECK(origin.x == ORIGIN.x);
    CHECK(size.x == Approx(SIZE.x));
}

TEST_CASE("A picture's channel toggles keep the last channel shown on", "[ui][canvas]") {
    KitHarness h;
    ui::Channels shown;
    h.set_ui([&] { ui::channel_toggles(shown, {.r = false, .g = false, .b = false, .a = false}, false); });
    h.step(2);
    // Without alpha there is no A to turn off, and it doesn't count.
    CHECK(h.find("A") == nullptr);
    h.click("R");
    h.click("G");
    CHECK_FALSE(shown.r);
    CHECK_FALSE(shown.g);
    h.click("B");
    CHECK(shown.b);
    h.click("R");
    CHECK(shown.r);
}

TEST_CASE("A fixed channel can't be turned off", "[ui][canvas]") {
    KitHarness h;
    ui::Channels shown;
    h.set_ui([&] { ui::channel_toggles(shown, {.r = false, .g = false, .b = true, .a = false}); });
    h.step(2);
    h.click("B");
    CHECK(shown.b);
}

TEST_CASE("A picture canvas's zoom asks to fit or to show it pixel for pixel", "[ui][canvas]") {
    KitHarness h;
    ui::CanvasView view;
    ui::ZoomAsk asked = ui::ZoomAsk::None;
    h.set_ui([&] {
        draw_canvas(view);
        if (const ui::ZoomAsk ask = ui::zoom_controls(view); ask != ui::ZoomAsk::None) asked = ask;
    });
    h.step(2);
    h.click(ui::icon::frame_view);
    CHECK(asked == ui::ZoomAsk::Fit);
    h.click("1:1");
    CHECK(asked == ui::ZoomAsk::PixelForPixel);
}

TEST_CASE("A canvas worked on at many sizes has zoom steps in its pill, and no 1:1", "[ui][canvas]") {
    KitHarness h;
    ui::CanvasView view;
    ui::ZoomAsk asked = ui::ZoomAsk::None;
    h.set_ui([&] {
        draw_canvas(view);
        const ui::ZoomAsk ask = ui::zoom_controls(view, {.steps = true, .pixel_for_pixel = false});
        if (ask != ui::ZoomAsk::None) asked = ask;
    });
    h.step(2);
    h.click(ui::icon::zoom_in);
    CHECK(asked == ui::ZoomAsk::In);
    h.click(ui::icon::zoom_out);
    CHECK(asked == ui::ZoomAsk::Out);
    h.click(ui::icon::frame_view);
    CHECK(asked == ui::ZoomAsk::Fit);
    CHECK_FALSE(h.find("1:1"));
}

TEST_CASE("A snap line is drawn solid across, in the text colour at 60 %", "[ui][canvas]") {
    KitHarness h;
    h.set_ui([] { ui::canvas_snap_line(ui::DrawList::window(), {20.0f, 40.0f}, {300.0f, 40.0f}); });
    h.step(1);
    ui::Color colour = ftk::theme::text();
    colour.a *= 0.6f;
    const uint32_t packed = ui::core::pack_colour(colour);
    int lines = 0;
    for (const auto& root : h.output().roots) {
        for (const auto& p : root.draw->primitives) {
            if (p.colour != packed) continue;
            ++lines;
            CHECK(p.kind == ui::core::PrimitiveKind::Segment);
            CHECK(p.shape == glm::vec4(20.0f, 40.0f, 300.0f, 40.0f));
            CHECK(p.thickness == Approx(1.0f));
        }
    }
    CHECK(lines == 1);
}

TEST_CASE("A measure line is dashed, its distance in a readout above it or beside it", "[ui][canvas]") {
    KitHarness h;
    bool upright = false;
    h.set_ui([&] {
        const glm::vec2 from{100.0f, 200.0f};
        const glm::vec2 to = upright ? glm::vec2{100.0f, 300.0f} : glm::vec2{300.0f, 200.0f};
        ui::canvas_measure_line(ui::DrawList::window(), from, to, "64", {0.0f, 0.0f}, {800.0f, 600.0f});
    });
    // The dashes, and the plate the readout sits on.
    const auto drawn = [&] {
        int dashes = 0;
        glm::vec2 plate_min{0.0f};
        glm::vec2 plate_max{0.0f};
        const uint32_t sunken = ui::core::pack_colour(ftk::theme::surface_sunken());
        for (const auto& root : h.output().roots) {
            for (const auto& p : root.draw->primitives) {
                if (p.kind == ui::core::PrimitiveKind::Segment && p.colour == ui::core::pack_colour(ftk::theme::text())) ++dashes;
                if (p.kind == ui::core::PrimitiveKind::Rect && p.colour == sunken) {
                    plate_min = p.min;
                    plate_max = p.max;
                }
            }
        }
        return std::tuple{dashes, plate_min, plate_max};
    };
    h.step(1);
    const auto [level_dashes, level_min, level_max] = drawn();
    CHECK(level_dashes > 20);
    CHECK((level_min.x + level_max.x) * 0.5f == Approx(200.0f).margin(1.0f));
    CHECK(level_max.y <= 200.0f);

    upright = true;
    h.step(1);
    const auto [upright_dashes, upright_min, upright_max] = drawn();
    CHECK(upright_dashes > 10);
    CHECK(upright_min.x > 100.0f);
    CHECK((upright_min.y + upright_max.y) * 0.5f == Approx(250.0f).margin(1.0f));
}
