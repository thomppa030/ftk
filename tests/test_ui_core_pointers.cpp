#include "ftk/test/ui_harness.hpp"
#include "ftk/ui/core/context.hpp"

#include <catch2/catch_test_macros.hpp>

#include <glm/vec2.hpp>

namespace core = ftk::ui::core;
using core::PointerEvent;
using core::PointerType;
using core::Rect;
using ftk::test::UiHarness;
using ftk::ui::MouseButton;

namespace {

// A widget `size` big at the cursor that takes the pointers as `sense` says.
core::Interaction widget(UiHarness& h, const char* label, glm::vec2 size = {100.0f, 24.0f}, core::Sense sense = {}) {
    core::Context& ui = h.ui();
    return ui.interact(ui.id(label), ui.layout().next(size), sense);
}

// What a widget's interactions came to over several frames.
struct Seen {
    int hovered{0};
    int pressed{0};
    int clicked{0};
    int double_clicked{0};
    int released{0};
    int cancelled{0};
    int dragging{0};

    void add(const core::Interaction& in) {
        hovered += in.hovered ? 1 : 0;
        pressed += in.pressed ? 1 : 0;
        clicked += in.clicked ? 1 : 0;
        double_clicked += in.double_clicked ? 1 : 0;
        released += in.released ? 1 : 0;
        cancelled += in.cancelled ? 1 : 0;
        dragging += in.dragging ? 1 : 0;
    }
};

PointerEvent touch(PointerEvent::Kind kind, glm::vec2 at, core::PointerId finger = 5) {
    return {.kind = kind, .type = PointerType::Touch, .id = finger, .sample = {.position = at}};
}

} // namespace

TEST_CASE("A click is a press and a release over the same widget", "[ui-core][pointers]") {
    UiHarness h;
    core::Interaction last;
    Seen save;
    h.set_ui([&] {
        last = widget(h, "Save");
        save.add(last);
        h.mark("save");
    });
    h.step();
    CHECK(h.rect("save").min == glm::vec2{8.0f, 8.0f});

    h.move_to(h.rect("save").centre());
    CHECK(last.hovered);
    CHECK(last.under);
    CHECK_FALSE(last.held);

    h.send({.kind = PointerEvent::Kind::Down, .sample = {.position = h.rect("save").centre()}});
    h.step();
    CHECK(last.pressed);
    CHECK(last.held);
    CHECK(h.ui().any_held());
    CHECK(last.pointer_type == PointerType::Mouse);
    h.send({.kind = PointerEvent::Kind::Up, .sample = {.position = h.rect("save").centre()}});
    h.step();
    CHECK(last.released);
    CHECK(last.clicked);
    CHECK(save.clicked == 1);
    h.step();
    CHECK_FALSE(h.ui().any_held());

    // Pressed on it and let go elsewhere: no click.
    h.drag(h.rect("save").centre(), {600.0f, 400.0f});
    CHECK(save.clicked == 1);
    CHECK(save.released == 2);
    CHECK(save.dragging > 0);
}

TEST_CASE("What is drawn later is on top, and takes the pointer there", "[ui-core][pointers]") {
    UiHarness h;
    Seen under;
    Seen over;
    h.set_ui([&] {
        const glm::vec2 at = h.ui().layout().cursor();
        under.add(widget(h, "under", {200.0f, 100.0f}));
        h.mark("under");
        h.ui().layout().set_cursor(at);
        over.add(widget(h, "over", {50.0f, 50.0f}));
        h.mark("over");
    });
    h.step();
    h.click("over");
    CHECK(over.clicked == 1);
    CHECK(under.clicked == 0);
    CHECK(under.hovered == 0);

    h.click_at(h.rect("under").max - glm::vec2{10.0f, 10.0f});
    CHECK(under.clicked == 1);
    CHECK(over.clicked == 1);
}

TEST_CASE("A press holds the pointer until release, and nothing else is hovered meanwhile", "[ui-core][pointers]") {
    UiHarness h;
    Seen a;
    Seen b;
    h.set_ui([&] {
        a.add(widget(h, "A"));
        h.mark("a");
        b.add(widget(h, "B"));
        h.mark("b");
    });
    h.step();
    h.drag(h.rect("a").centre(), h.rect("b").centre());
    CHECK(a.pressed == 1);
    CHECK(a.dragging >= 3);
    CHECK(a.released == 1);
    CHECK(a.clicked == 0);
    CHECK(b.hovered == 0);
    CHECK(b.pressed == 0);
    // Free again: B hovers now the mouse rests on it.
    h.step();
    CHECK(b.hovered > 0);
}

TEST_CASE("A double click is a second press in quick succession", "[ui-core][pointers]") {
    UiHarness h;
    Seen row;
    h.set_ui([&] {
        row.add(widget(h, "row"));
        h.mark("row");
    });
    h.step();
    h.double_click("row");
    CHECK(row.clicked == 2);
    CHECK(row.double_clicked == 1);
    // Half a second later, a click is a click again.
    h.step(30);
    h.click("row");
    CHECK(row.double_clicked == 1);
}

TEST_CASE("A widget takes presses only from the buttons it senses", "[ui-core][pointers]") {
    UiHarness h;
    Seen left;
    Seen right;
    core::Interaction last_right;
    h.set_ui([&] {
        left.add(widget(h, "left"));
        h.mark("left");
        last_right = widget(h, "right", {100.0f, 24.0f}, {.left = false, .right = true});
        right.add(last_right);
        h.mark("right");
    });
    h.step();
    h.click("left", MouseButton::Right);
    CHECK(left.pressed == 0);
    h.click("right", MouseButton::Left);
    CHECK(right.pressed == 0);
    h.click("right", MouseButton::Right);
    CHECK(right.clicked == 1);
    CHECK(last_right.button == MouseButton::Right);
}

TEST_CASE("A press brings its panel to the front, and a panel hides what is beneath it", "[ui-core][pointers]") {
    UiHarness h;
    Seen in_back;
    h.set_ui([&] {
        core::Context& ui = h.ui();
        ui.begin_panel("back", {.min = {100.0f, 100.0f}, .max = {300.0f, 300.0f}}, {.padding = {0.0f, 0.0f}});
        ui.draw_list().rect_filled({100.0f, 100.0f}, {300.0f, 300.0f}, {1.0f, 0.0f, 0.0f, 1.0f});
        ui.layout().set_cursor({200.0f, 200.0f});
        in_back.add(widget(h, "covered", {50.0f, 50.0f}));
        h.mark("covered");
        ui.end_panel();
        ui.begin_panel("front", {.min = {150.0f, 150.0f}, .max = {350.0f, 350.0f}});
        ui.draw_list().rect_filled({150.0f, 150.0f}, {350.0f, 350.0f}, {0.0f, 0.0f, 1.0f, 1.0f});
        ui.end_panel();
    });
    h.step();
    const auto colour_on_top = [&] { return h.output().roots[0].draw->primitives.back().colour; };
    CHECK(colour_on_top() == core::pack_colour({0.0f, 0.0f, 1.0f, 1.0f}));

    // Under the front panel: not reached.
    h.click("covered");
    CHECK(in_back.pressed == 0);
    CHECK(in_back.hovered == 0);

    // A press on the back panel where it shows raises it.
    h.click_at({120.0f, 120.0f});
    CHECK(colour_on_top() == core::pack_colour({1.0f, 0.0f, 0.0f, 1.0f}));
    h.click("covered");
    CHECK(in_back.clicked == 1);
}

TEST_CASE("Disabled widgets take nothing, but still cover what is beneath them", "[ui-core][pointers]") {
    UiHarness h;
    Seen off;
    Seen beneath;
    h.set_ui([&] {
        core::Context& ui = h.ui();
        const glm::vec2 at = ui.layout().cursor();
        beneath.add(widget(h, "beneath"));
        ui.layout().set_cursor(at);
        ui.push_disabled();
        off.add(widget(h, "Bake"));
        h.mark("bake");
        ui.pop_disabled();
    });
    h.step();
    h.click("bake");
    CHECK(off.hovered == 0);
    CHECK(off.pressed == 0);
    CHECK(beneath.pressed == 0);
}

TEST_CASE("Touch has no hover, a tap clicks, and a cancelled touch clicks nothing", "[ui-core][pointers]") {
    UiHarness h;
    Seen button;
    core::Interaction last;
    h.set_ui([&] {
        last = widget(h, "button");
        button.add(last);
        h.mark("button");
    });
    h.step();
    const glm::vec2 at = h.rect("button").centre();

    // Down and up within one frame.
    h.send(touch(PointerEvent::Kind::Down, at));
    h.send(touch(PointerEvent::Kind::Up, at));
    h.step();
    CHECK(button.clicked == 1);
    CHECK(button.hovered == 0);
    CHECK(last.pointer_type == PointerType::Touch);
    CHECK(h.ui().input_mode() == PointerType::Touch);
    h.step();
    CHECK(h.ui().pointer_count() == 0);

    // Down, then taken back by the system: dropped, not clicked.
    h.send(touch(PointerEvent::Kind::Down, at, 6));
    h.step();
    CHECK(last.held);
    h.send(touch(PointerEvent::Kind::Cancel, at, 6));
    h.step();
    CHECK(last.cancelled);
    CHECK(last.released);
    CHECK_FALSE(last.clicked);
    CHECK(button.clicked == 1);

    // A mouse press makes the mouse the input mode again.
    h.click("button");
    CHECK(h.ui().input_mode() == PointerType::Mouse);
}

TEST_CASE("A canvas gets every sample since the last frame", "[ui-core][pointers]") {
    UiHarness h;
    core::Interaction last;
    h.set_ui([&] {
        last = widget(h, "paper", {400.0f, 300.0f});
        h.mark("paper");
    });
    h.step();
    const auto pen = [&](PointerEvent::Kind kind, glm::vec2 at, float pressure) {
        h.send({.kind = kind, .type = PointerType::Pen, .id = 7, .sample = {.position = at, .pressure = pressure, .tilt = {0.2f, 0.0f}}});
    };
    pen(PointerEvent::Kind::Move, {50.0f, 50.0f}, 0.0f);
    pen(PointerEvent::Kind::Down, {52.0f, 50.0f}, 0.2f);
    pen(PointerEvent::Kind::Move, {60.0f, 52.0f}, 0.4f);
    pen(PointerEvent::Kind::Move, {70.0f, 55.0f}, 0.6f);
    pen(PointerEvent::Kind::Move, {80.0f, 60.0f}, 0.5f);
    h.step();
    CHECK(last.pressed);
    CHECK(last.pointer_type == PointerType::Pen);
    CHECK(last.pressure == 0.5f);
    const core::Pointer* stylus = h.ui().find_pointer(PointerType::Pen, 7);
    REQUIRE(stylus != nullptr);
    REQUIRE(stylus->samples.size() == 5);
    CHECK(stylus->samples[2].pressure == 0.4f);
    CHECK(stylus->samples[4].position == glm::vec2{80.0f, 60.0f});
    CHECK(stylus->samples[1].tilt.x == 0.2f);
    // The next frame has only what came after.
    pen(PointerEvent::Kind::Move, {90.0f, 61.0f}, 0.5f);
    h.step();
    CHECK(stylus->samples.size() == 1);
    CHECK(last.held);
    CHECK(last.dragging);
}

TEST_CASE("A widget that goes while it holds the pointer lets it go", "[ui-core][pointers]") {
    UiHarness h;
    bool shown = true;
    Seen gone;
    h.set_ui([&] {
        if (shown) {
            gone.add(widget(h, "gone"));
            h.mark("gone");
        }
    });
    h.step();
    h.move_to(h.rect("gone").centre());
    h.send({.kind = PointerEvent::Kind::Down, .sample = {.position = h.rect("gone").centre()}});
    h.step();
    CHECK(h.ui().any_held());
    shown = false;
    h.step(2);
    CHECK_FALSE(h.ui().any_held());
    shown = true;
    h.send({.kind = PointerEvent::Kind::Up, .sample = {.position = h.rect("gone").centre()}});
    h.step();
    CHECK(gone.clicked == 0);
}

TEST_CASE("The wheel scrolls the innermost region under the mouse that can still scroll", "[ui-core][pointers]") {
    UiHarness h;
    glm::vec2 outer_scroll{};
    glm::vec2 inner_scroll{};
    int inner_rows = 10;
    h.set_ui([&] {
        core::Context& ui = h.ui();
        ui.begin_region("outer", {300.0f, 150.0f});
        outer_scroll = ui.scroll();
        ui.begin_region("inner", {280.0f, 60.0f});
        h.mark("inner");
        inner_scroll = ui.scroll();
        for (int i = 0; i < inner_rows; ++i) (void)widget(h, "row");
        ui.end_region();
        for (int i = 0; i < 10; ++i) (void)widget(h, "item");
        ui.end_region();
    });
    h.step(2);
    // Inside the inner region, which begins at the outer's top-left.
    const glm::vec2 at{100.0f, 30.0f};
    CHECK(h.rect("inner").contains(at));

    // Down one notch over the inner region: it scrolls, smoothly, and the
    // outer doesn't.
    h.wheel(at, -1.0f);
    CHECK(h.output().next.now);
    h.step(30);
    CHECK(inner_scroll.y == 48.0f);
    CHECK(outer_scroll.y == 0.0f);
    CHECK(h.output().next.waits_for_input());

    // An inner region with nothing to scroll passes the wheel out.
    inner_rows = 1;
    h.step(2);
    h.wheel(at, -1.0f);
    h.step(30);
    CHECK(outer_scroll.y == 48.0f);
}

TEST_CASE("When what is under a still pointer changes, another frame finds it", "[ui-core][pointers]") {
    UiHarness h;
    bool moved = false;
    Seen target;
    h.set_ui([&] {
        if (moved) h.ui().layout().set_cursor({400.0f, 400.0f});
        target.add(widget(h, "target"));
        h.mark("target");
        if (target.hovered > 0) h.ui().set_cursor_shape(ftk::ui::MouseCursor::Hand);
    });
    h.step();
    h.move_to(h.rect("target").centre());
    CHECK(target.hovered == 1);
    CHECK(h.output().roots[0].cursor == ftk::ui::MouseCursor::Hand);
    h.step();
    CHECK(h.output().next.waits_for_input());
    moved = true;
    h.step();
    CHECK(h.output().next.now);
    h.step();
    CHECK(h.output().next.waits_for_input());
}
