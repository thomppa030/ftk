#include "ftk/ui/core/context.hpp"
#include "ftk/ui/core/layout.hpp"

#include <catch2/catch_test_macros.hpp>

#include <glm/vec2.hpp>

namespace core = ftk::ui::core;
using core::Rect;

namespace {

constexpr ftk::ui::Color GREY{0.5f, 0.5f, 0.5f, 1.0f};

// A frame of a 400 by 300 root, drawn by `draw`. Returns the frame's
// output.
const core::FrameOutput& frame(core::Context& ui, auto draw) {
    ui.begin_frame(static_cast<double>(ui.frame()));
    ui.begin_root(1, {400.0f, 300.0f}, 1.0f);
    draw();
    ui.end_root();
    return ui.end_frame();
}

// An item `size` big, drawn as a rectangle, so what is seen can be counted.
Rect item(core::Context& ui, glm::vec2 size) {
    const Rect r = ui.layout().next(size);
    ui.draw_list().rect_filled(r.min, r.max, GREY);
    return r;
}

} // namespace

TEST_CASE("Items go one under the other, or on the same line when asked", "[ui-core][layout]") {
    core::Layout layout;
    layout.set_style({.item_spacing = {8.0f, 4.0f}, .empty_line = 16.0f});
    layout.begin_area({.min = {10.0f, 10.0f}, .max = {210.0f, 110.0f}});

    const Rect a = layout.next({50.0f, 20.0f});
    CHECK(a == Rect{.min = {10.0f, 10.0f}, .max = {60.0f, 30.0f}});
    const Rect b = layout.next({30.0f, 10.0f});
    CHECK(b.min == glm::vec2{10.0f, 34.0f});

    layout.same_line();
    const Rect c = layout.next({40.0f, 24.0f});
    CHECK(c.min == glm::vec2{48.0f, 34.0f});
    // The next line is below the taller of the two.
    CHECK(layout.cursor() == glm::vec2{10.0f, 62.0f});
    CHECK(layout.last_item() == c);

    layout.same_line(0.0f);
    CHECK(layout.cursor() == glm::vec2{88.0f, 34.0f});
    CHECK(layout.space_left() == glm::vec2{122.0f, 76.0f});
    CHECK(layout.resolve_width(core::FILL) == 122.0f);
    CHECK(layout.resolve_width(-22.0f) == 100.0f);
    CHECK(layout.resolve_width(30.0f) == 30.0f);
    // new_line() takes the same line back; another leaves an empty line.
    layout.new_line();
    CHECK(layout.cursor() == glm::vec2{10.0f, 62.0f});
    layout.new_line();
    CHECK(layout.cursor() == glm::vec2{10.0f, 78.0f});
    layout.spacing();
    CHECK(layout.cursor() == glm::vec2{10.0f, 82.0f});

    layout.set_cursor({100.0f, 90.0f});
    CHECK(layout.next({5.0f, 5.0f}).min == glm::vec2{100.0f, 90.0f});
    // Measured from where the items began.
    CHECK(layout.end_area() == glm::vec2{95.0f, 85.0f});
}

TEST_CASE("The layout tells whether an item goes on the line of the one before it", "[ui-core][layout]") {
    core::Layout layout;
    layout.begin_area({.min = {0.0f, 0.0f}, .max = {300.0f, 200.0f}});
    CHECK_FALSE(layout.continues_line());
    (void)layout.next({50.0f, 20.0f});
    CHECK_FALSE(layout.last_continued_line());
    CHECK_FALSE(layout.continues_line());

    layout.same_line();
    CHECK(layout.continues_line());
    (void)layout.next({50.0f, 20.0f});
    CHECK(layout.last_continued_line());
    CHECK_FALSE(layout.continues_line());

    // A group begun on the line: its first item is on it too.
    layout.same_line();
    layout.begin_group();
    CHECK(layout.continues_line());
    (void)layout.next({20.0f, 20.0f});
    CHECK(layout.last_continued_line());
    CHECK_FALSE(layout.continues_line());
    (void)layout.end_group();
    CHECK(layout.last_continued_line());

    layout.same_line();
    layout.new_line();
    CHECK_FALSE(layout.continues_line());
    (void)layout.end_area();
}

TEST_CASE("A group is one item to what follows it", "[ui-core][layout]") {
    core::Layout layout;
    layout.begin_area({.min = {0.0f, 0.0f}, .max = {300.0f, 200.0f}});
    layout.begin_group();
    (void)layout.next({60.0f, 20.0f});
    (void)layout.next({90.0f, 20.0f});
    const Rect group = layout.end_group();
    CHECK(group == Rect{.min = {0.0f, 0.0f}, .max = {90.0f, 44.0f}});
    CHECK(layout.last_item() == group);
    layout.same_line();
    const Rect beside = layout.next({10.0f, 10.0f});
    CHECK(beside.min == glm::vec2{98.0f, 0.0f});
    CHECK(layout.cursor().y == 48.0f);
    (void)layout.end_area();
}

TEST_CASE("A container is hidden the first frame, then placed by what it measured", "[ui-core][layout]") {
    core::Context ui;
    Rect pill;
    Rect button;
    const auto status_bar = [&] {
        ui.begin_container("status", {.axis = core::Axis::Row, .gap = 4.0f, .centre_across = true});
        pill = item(ui, {80.0f, 16.0f});
        ui.spring();
        (void)item(ui, {20.0f, 24.0f});
        button = item(ui, {60.0f, 24.0f});
        ui.end_container();
        (void)item(ui, {10.0f, 10.0f});  // under it, seen every frame
    };

    // Not seen, and another frame asked for.
    const core::FrameOutput& first = frame(ui, [&] {
        status_bar();
        CHECK(ui.hidden() == false);
    });
    CHECK(first.next.now);
    CHECK(first.roots[0].draw->primitives.size() == 1);

    // Seen: the right group ends at the right edge, the pill centred in the
    // row's height. Nothing changed, so no frame is asked for.
    const core::FrameOutput& second = frame(ui, status_bar);
    CHECK(second.roots[0].draw->primitives.size() == 4);
    CHECK(button.max.x == 400.0f);
    CHECK(button.min.x == 340.0f);
    CHECK(pill.min.y == 4.0f);
    CHECK(second.next.waits_for_input());

    // A child grown: placed by last frame's sizes, and another frame asked
    // for to place it by this one's.
    const auto wider = [&] {
        ui.begin_container("status", {.axis = core::Axis::Row, .gap = 4.0f, .centre_across = true});
        pill = item(ui, {80.0f, 16.0f});
        ui.spring();
        (void)item(ui, {20.0f, 24.0f});
        button = item(ui, {100.0f, 24.0f});
        ui.end_container();
    };
    CHECK(frame(ui, wider).next.now);
    CHECK(button.max.x == 440.0f);
    CHECK(frame(ui, wider).next.waits_for_input());
    CHECK(button.max.x == 400.0f);
}

TEST_CASE("Two springs centre what is between them, along either axis", "[ui-core][layout]") {
    core::Context ui;
    Rect across;
    Rect down;
    const auto draw = [&] {
        ui.begin_container("bar");
        ui.spring();
        across = item(ui, {100.0f, 20.0f});
        ui.spring();
        ui.end_container();
        // An empty state: centred both ways in the room left.
        ui.begin_container("empty", {.axis = core::Axis::Column, .centre_across = true});
        ui.spring();
        down = item(ui, {120.0f, 40.0f});
        ui.spring();
        ui.end_container();
    };
    (void)frame(ui, draw);
    (void)frame(ui, draw);
    CHECK(across.min.x == 150.0f);
    CHECK(down.min.x == 140.0f);
    // The column starts under the bar, 24 down, and fills the 276 left.
    CHECK(down.min.y == 24.0f + 0.5f * (276.0f - 40.0f));
}

TEST_CASE("A wrapping row goes onto another line when a child doesn't fit, and is never hidden", "[ui-core][layout]") {
    core::Context ui;
    std::vector<Rect> tools;
    const core::FrameOutput& out = frame(ui, [&] {
        ui.layout().set_cursor({250.0f, 0.0f});
        ui.begin_container("tools", {.gap = 2.0f, .wrap = true});
        for (int i = 0; i < 5; ++i) tools.push_back(item(ui, {48.0f, 48.0f}));
        ui.end_container();
    });
    REQUIRE(tools.size() == 5);
    CHECK(tools[1].min == glm::vec2{300.0f, 0.0f});
    CHECK(tools[2].min == glm::vec2{350.0f, 0.0f});
    CHECK(tools[3].min == glm::vec2{250.0f, 52.0f});
    CHECK(out.roots[0].draw->primitives.size() == 5);
    CHECK(out.next.waits_for_input());
}

TEST_CASE("Panels hold their items inside their padding, drawn over what came before", "[ui-core][layout]") {
    core::Context ui;
    Rect in_first;
    Rect in_popup;
    ftk::ui::Id first_id = 0;
    ftk::ui::Id popup_id = 0;
    const core::FrameOutput& out = frame(ui, [&] {
        (void)item(ui, {400.0f, 300.0f});  // the root's own, under everything
        ui.begin_panel("Layers", {.min = {10.0f, 10.0f}, .max = {110.0f, 210.0f}});
        in_first = item(ui, {50.0f, 20.0f});
        first_id = ui.id("Opacity");
        // A popup begun from inside: over the panel and not clipped by it.
        ui.begin_panel("menu", {.min = {100.0f, 50.0f}, .max = {300.0f, 100.0f}}, {.padding = {4.0f, 4.0f}});
        in_popup = item(ui, {150.0f, 20.0f});
        popup_id = ui.id("Opacity");
        ui.end_panel();
        (void)item(ui, {50.0f, 20.0f});
        ui.end_panel();
    });
    CHECK(in_first.min == glm::vec2{18.0f, 18.0f});
    CHECK(in_popup.min == glm::vec2{104.0f, 54.0f});
    CHECK(first_id != popup_id);
    const auto& primitives = out.roots[0].draw->primitives;
    const auto& clips = out.roots[0].draw->clips;
    REQUIRE(primitives.size() == 4);
    // The root's, then the panel's two, then the popup's.
    CHECK(primitives[0].max == glm::vec2{400.0f, 300.0f});
    CHECK(primitives[1].min == in_first.min);
    CHECK(primitives[3].min == in_popup.min);
    CHECK(clips[primitives[3].clip].max == glm::vec2{300.0f, 100.0f});
}

TEST_CASE("A region scrolls what it holds, as far as that reaches", "[ui-core][layout]") {
    core::Context ui;
    Rect first;
    Rect region;
    glm::vec2 scroll_max{};
    std::size_t rows = 10;
    float scroll_to = -1.0f;
    float scrolled = 0.0f;
    const auto draw = [&] {
        ui.begin_region("list", {200.0f, 100.0f});
        if (scroll_to >= 0.0f) {
            ui.set_scroll({0.0f, scroll_to});
            scrolled = ui.scroll().y;
            scroll_to = -1.0f;
        }
        for (std::size_t i = 0; i < rows; ++i) {
            const Rect row = item(ui, {180.0f, 20.0f});
            if (i == 0) first = row;
        }
        scroll_max = ui.scroll_max();
        ui.end_region();
        region = ui.layout().last_item();
    };
    (void)frame(ui, draw);
    CHECK(region == Rect{.min = {0.0f, 0.0f}, .max = {200.0f, 100.0f}});
    // Ten rows of 20 with 4 between: 236 tall, 136 more than shows.
    (void)frame(ui, draw);
    CHECK(scroll_max == glm::vec2{0.0f, 136.0f});

    // Scrolled past the end: as far as it goes, drawn so from the next
    // frame, which is asked for.
    scroll_to = 500.0f;
    CHECK(frame(ui, draw).next.now);
    CHECK(scrolled == 136.0f);
    CHECK(first.min.y == 0.0f);
    (void)frame(ui, draw);
    CHECK(first.min.y == -136.0f);

    // Fewer rows: scrolled back to what they reach, and a frame asked for.
    rows = 5;
    CHECK(frame(ui, draw).next.now);
    CHECK(frame(ui, draw).next.waits_for_input());
    CHECK(first.min.y == -16.0f);
}

TEST_CASE("A region's padding keeps what it holds in from its edges", "[ui-core][layout]") {
    core::Context ui;
    Rect first;
    Rect inside;
    glm::vec2 scroll_max{};
    const auto draw = [&] {
        ui.begin_region("pane", {200.0f, 100.0f}, {.padding = {10.0f, 8.0f}});
        inside = ui.area_inside();
        for (int i = 0; i < 5; ++i) {
            const Rect row = item(ui, {ui.layout().resolve_width(core::FILL), 20.0f});
            if (i == 0) first = row;
        }
        scroll_max = ui.scroll_max();
        ui.end_region();
    };
    (void)frame(ui, draw);
    (void)frame(ui, draw);
    CHECK(inside == Rect{.min = {10.0f, 8.0f}, .max = {190.0f, 92.0f}});
    CHECK(first == Rect{.min = {10.0f, 8.0f}, .max = {190.0f, 28.0f}});
    // Five rows of 20 with 4 between: 116 tall in 84 of room.
    CHECK(scroll_max == glm::vec2{0.0f, 32.0f});
}
