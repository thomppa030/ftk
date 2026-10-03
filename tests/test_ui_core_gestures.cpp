#include "ftk/test/ui_harness.hpp"
#include "ftk/ui/core/context.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/vec2.hpp>

#include <vector>

namespace core = ftk::ui::core;
using core::PointerEvent;
using core::PointerType;
using ftk::test::UiHarness;

namespace {

// A canvas filling most of the harness's panel, reporting each frame.
struct Canvas {
    core::Sense sense{.canvas = true};
    std::vector<core::Interaction> frames;

    void draw(UiHarness& h) {
        core::Context& ui = h.ui();
        frames.push_back(ui.interact(ui.id("canvas"), ui.layout().next({600.0f, 400.0f}), sense));
        h.mark("canvas");
    }
    [[nodiscard]] int count(bool core::Interaction::*flag) const {
        int n = 0;
        for (const core::Interaction& in : frames) n += (in.*flag) ? 1 : 0;
        return n;
    }
};

} // namespace

TEST_CASE("Two fingers pinch: their spread and their middle, frame by frame", "[ui-core][gestures]") {
    UiHarness h;
    Canvas canvas;
    h.set_ui([&] { canvas.draw(h); });
    h.step();
    canvas.frames.clear();

    h.pinch({300.0f, 200.0f}, 100.0f, 200.0f);
    float spread = 1.0f;
    glm::vec2 panned{0.0f};
    int began = 0;
    int strokes = 0;
    for (const core::Interaction& in : canvas.frames) {
        if (in.gesture.touches < 2) continue;
        spread *= in.gesture.scale;
        panned += in.gesture.pan;
        began += in.gesture.began ? 1 : 0;
        strokes += in.stroke ? 1 : 0;
        CHECK(in.gesture.centre.x == Catch::Approx(300.0f));
    }
    CHECK(spread == Catch::Approx(2.0f));
    CHECK(panned.x == Catch::Approx(0.0f).margin(1e-4));
    CHECK(began == 1);
    CHECK(strokes == 0);
    // Moved apart: not a tap.
    int taps = 0;
    for (const core::Interaction& in : canvas.frames) taps += in.gesture.taps;
    CHECK(taps == 0);
}

TEST_CASE("Two fingers moved together pan", "[ui-core][gestures]") {
    UiHarness h;
    Canvas canvas;
    h.set_ui([&] { canvas.draw(h); });
    h.step();
    canvas.frames.clear();
    const auto fingers = [&](PointerEvent::Kind kind, float x) {
        h.send({.kind = kind, .type = PointerType::Touch, .id = 1, .sample = {.position = {x, 200.0f}}});
        h.send({.kind = kind, .type = PointerType::Touch, .id = 2, .sample = {.position = {x + 80.0f, 200.0f}}});
    };
    fingers(PointerEvent::Kind::Down, 200.0f);
    h.step();
    for (int i = 1; i <= 3; ++i) {
        fingers(PointerEvent::Kind::Move, 200.0f + 10.0f * static_cast<float>(i));
        h.step();
    }
    fingers(PointerEvent::Kind::Up, 230.0f);
    h.step();
    glm::vec2 panned{0.0f};
    for (const core::Interaction& in : canvas.frames) {
        panned += in.gesture.pan;
        if (in.gesture.touches >= 2) CHECK(in.gesture.scale == Catch::Approx(1.0f));
    }
    CHECK(panned.x == Catch::Approx(30.0f));
    CHECK(panned.y == Catch::Approx(0.0f));
}

TEST_CASE("Fingers lifted together, quickly and still, are a tap of that many", "[ui-core][gestures]") {
    UiHarness h;
    Canvas canvas;
    h.set_ui([&] { canvas.draw(h); });
    h.step();
    const auto taps = [&] {
        int n = 0;
        for (const core::Interaction& in : canvas.frames) n = std::max(n, in.gesture.taps);
        canvas.frames.clear();
        return n;
    };
    canvas.frames.clear();
    h.tap_at({300.0f, 200.0f}, 2);
    CHECK(taps() == 2);
    h.tap_at({300.0f, 200.0f}, 3);
    CHECK(taps() == 3);
    // One finger is a tap of the ordinary kind: a click.
    h.tap_at({300.0f, 200.0f}, 1);
    CHECK(canvas.count(&core::Interaction::clicked) == 1);
    CHECK(taps() == 0);

    // Held too long to be a tap.
    h.send({.kind = PointerEvent::Kind::Down, .type = PointerType::Touch, .id = 50, .sample = {.position = {300.0f, 200.0f}}});
    h.send({.kind = PointerEvent::Kind::Down, .type = PointerType::Touch, .id = 51, .sample = {.position = {320.0f, 200.0f}}});
    h.step(30);
    h.send({.kind = PointerEvent::Kind::Up, .type = PointerType::Touch, .id = 50, .sample = {.position = {300.0f, 200.0f}}});
    h.send({.kind = PointerEvent::Kind::Up, .type = PointerType::Touch, .id = 51, .sample = {.position = {320.0f, 200.0f}}});
    h.step();
    CHECK(taps() == 0);
}

TEST_CASE("A finger held still is a long press, at a deadline, and doesn't click", "[ui-core][gestures]") {
    UiHarness h;
    std::vector<core::Interaction> frames;
    h.set_ui([&] {
        core::Context& ui = h.ui();
        frames.push_back(ui.interact(ui.id("node"), ui.layout().next({120.0f, 40.0f})));
        h.mark("node");
    });
    h.step();
    const double pressed_at = h.time();
    h.send({.kind = PointerEvent::Kind::Down, .type = PointerType::Touch, .id = 9, .sample = {.position = h.rect("node").centre()}});
    h.step();
    // The frame after the press, which a frame that took input asks for;
    // then asleep until the press would be long, not drawing every frame.
    CHECK(h.output().next.now);
    h.step();
    REQUIRE(h.output().next.at.has_value());
    CHECK(*h.output().next.at == Catch::Approx(pressed_at + 0.5));
    CHECK_FALSE(h.output().next.now);
    frames.clear();
    h.step(29);
    h.send({.kind = PointerEvent::Kind::Up, .type = PointerType::Touch, .id = 9, .sample = {.position = h.rect("node").centre()}});
    h.step();
    int long_pressed = 0;
    int held_long = 0;
    int clicked = 0;
    for (const core::Interaction& in : frames) {
        long_pressed += in.long_pressed ? 1 : 0;
        held_long += in.held_long ? 1 : 0;
        clicked += in.clicked ? 1 : 0;
    }
    CHECK(long_pressed == 1);
    CHECK(held_long > 1);
    CHECK(clicked == 0);

    // The mouse held as long is still a click: it hovers instead.
    frames.clear();
    h.move_to(h.rect("node").centre());
    h.send({.kind = PointerEvent::Kind::Down, .sample = {.position = h.rect("node").centre()}});
    h.step(40);
    h.send({.kind = PointerEvent::Kind::Up, .sample = {.position = h.rect("node").centre()}});
    h.step();
    clicked = 0;
    for (const core::Interaction& in : frames) {
        clicked += in.clicked ? 1 : 0;
        CHECK_FALSE(in.long_pressed);
    }
    CHECK(clicked == 1);
}

TEST_CASE("While a pen is near, fingers on a canvas are a palm", "[ui-core][gestures]") {
    UiHarness h;
    Canvas canvas;
    std::vector<core::Interaction> button;
    h.set_ui([&] {
        canvas.draw(h);
        core::Context& ui = h.ui();
        button.push_back(ui.interact(ui.id("button"), ui.layout().next({100.0f, 24.0f})));
        h.mark("button");
    });
    h.step();
    const glm::vec2 on_canvas{200.0f, 200.0f};
    const auto pen = [&](PointerEvent::Kind kind, glm::vec2 at) {
        h.send({.kind = kind, .type = PointerType::Pen, .id = 3, .sample = {.position = at, .pressure = 0.5f}});
    };

    // A finger already drawing when the pen comes near: dropped.
    canvas.frames.clear();
    h.send({.kind = PointerEvent::Kind::Down, .type = PointerType::Touch, .id = 1, .sample = {.position = on_canvas}});
    h.step();
    CHECK(canvas.frames.back().stroke);
    pen(PointerEvent::Kind::Move, {400.0f, 300.0f});
    h.step();
    CHECK(canvas.frames.back().cancelled);
    CHECK_FALSE(canvas.frames.back().stroke);
    h.step();
    CHECK_FALSE(canvas.frames.back().held);

    // A finger coming down while the pen is near: refused, though it lies
    // on the canvas.
    canvas.frames.clear();
    h.send({.kind = PointerEvent::Kind::Down, .type = PointerType::Touch, .id = 2, .sample = {.position = on_canvas}});
    h.step(3);
    CHECK(canvas.count(&core::Interaction::held) == 0);
    // Other widgets still take fingers.
    h.tap("button");
    int buttons = 0;
    for (const core::Interaction& in : button) buttons += in.clicked ? 1 : 0;
    CHECK(buttons == 1);

    // The pen draws.
    canvas.frames.clear();
    pen(PointerEvent::Kind::Down, {300.0f, 250.0f});
    h.step();
    pen(PointerEvent::Kind::Move, {310.0f, 255.0f});
    h.step();
    CHECK(canvas.frames.back().stroke);
    CHECK(canvas.frames.back().pointer_type == PointerType::Pen);
    pen(PointerEvent::Kind::Up, {310.0f, 255.0f});
    pen(PointerEvent::Kind::Leave, {310.0f, 255.0f});
    h.step();

    // The palm stays refused until it is lifted; a new finger draws again.
    canvas.frames.clear();
    h.step();
    CHECK(canvas.count(&core::Interaction::held) == 0);
    h.send({.kind = PointerEvent::Kind::Up, .type = PointerType::Touch, .id = 2, .sample = {.position = on_canvas}});
    h.step();
    h.send({.kind = PointerEvent::Kind::Down, .type = PointerType::Touch, .id = 4, .sample = {.position = on_canvas}});
    h.step();
    CHECK(canvas.frames.back().stroke);
}

TEST_CASE("Where only the pen draws, a finger makes gestures and no stroke", "[ui-core][gestures]") {
    UiHarness h;
    Canvas canvas;
    canvas.sense.pen_only = true;
    h.set_ui([&] { canvas.draw(h); });
    h.step();
    canvas.frames.clear();
    h.send({.kind = PointerEvent::Kind::Down, .type = PointerType::Touch, .id = 1, .sample = {.position = {200.0f, 200.0f}}});
    h.step();
    CHECK(canvas.frames.back().held);
    CHECK_FALSE(canvas.frames.back().stroke);
    h.send({.kind = PointerEvent::Kind::Up, .type = PointerType::Touch, .id = 1, .sample = {.position = {200.0f, 200.0f}}});
    h.step();

    // Where fingers may draw, a second finger ends the stroke: a gesture.
    canvas.sense.pen_only = false;
    canvas.frames.clear();
    h.send({.kind = PointerEvent::Kind::Down, .type = PointerType::Touch, .id = 5, .sample = {.position = {200.0f, 200.0f}}});
    h.step();
    CHECK(canvas.frames.back().stroke);
    h.send({.kind = PointerEvent::Kind::Down, .type = PointerType::Touch, .id = 6, .sample = {.position = {260.0f, 200.0f}}});
    h.step();
    CHECK(canvas.frames.back().gesture.began);
    CHECK_FALSE(canvas.frames.back().stroke);
}

TEST_CASE("A pen stroke reaches the canvas sample by sample", "[ui-core][gestures]") {
    UiHarness h;
    Canvas canvas;
    std::vector<core::PointerSample> stroke;
    h.set_ui([&] {
        canvas.draw(h);
        const core::Interaction& in = canvas.frames.back();
        if (!in.stroke) return;
        const core::Pointer* pen = h.ui().find_pointer(in.pointer_type, in.pointer);
        if (pen != nullptr) stroke.insert(stroke.end(), pen->samples.begin(), pen->samples.end());
    });
    h.step();
    const std::vector<glm::vec2> points{{100.0f, 100.0f}, {120.0f, 110.0f}, {140.0f, 125.0f}, {160.0f, 130.0f}};
    h.pen_stroke(points, 0.7f);
    // Down, the moves, and the lift.
    REQUIRE(stroke.size() == points.size() + 1);
    CHECK(stroke.front().position == points.front());
    CHECK(stroke[2].position == points[2]);
    CHECK(stroke[1].pressure == Catch::Approx(0.7f));
    CHECK(stroke.back().pressure == 0.0f);
}
