#include "kit_harness.hpp"

#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/drag_drop.hpp"
#include "ftk/ui/kit/feedback.hpp"
#include "ftk/ui/kit/field.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/image.hpp"
#include "ftk/ui/kit/input.hpp"
#include "ftk/ui/kit/item.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/panel.hpp"
#include "ftk/ui/kit/region.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/selectable.hpp"
#include "ftk/ui/kit/state.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/tooltip.hpp"
#include "ftk/ui/kit/viewport_toolbar.hpp"
#include "ftk/ui/kit/window.hpp"
#include "ftk/ui/theme.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <optional>

#include <algorithm>
#include <array>
#include <string>
#include <utility>
#include <vector>

using ftk::test::KitHarness;
namespace ui = ftk::ui;

namespace {

glm::vec2 centre(const KitHarness& h, const std::string& name) {
    return h.rect(name).centre();
}

} // namespace

TEST_CASE("The cursor is where the next item goes, and reserve takes room there", "[ui][kit]") {
    KitHarness h;
    glm::vec2 cursor_after_set{};
    glm::vec2 reserved_min{};
    glm::vec2 reserved_max{};
    glm::vec2 space{};
    h.set_ui([&] {
        space = ui::space_left();
        ui::set_cursor({100.0f, 200.0f});
        cursor_after_set = ui::cursor();
        ui::reserve({50.0f, 20.0f});
        reserved_min = ui::item_min();
        reserved_max = ui::item_max();
    });
    h.step(2);
    CHECK(cursor_after_set == glm::vec2{100.0f, 200.0f});
    CHECK(reserved_min == glm::vec2{100.0f, 200.0f});
    CHECK(reserved_max == glm::vec2{150.0f, 220.0f});
    CHECK(space.x > 700.0f);
    CHECK(space.y > 500.0f);
}

TEST_CASE("A group is one item", "[ui][kit]") {
    KitHarness h;
    glm::vec2 group_min{};
    glm::vec2 group_max{};
    float next_width = 0.0f;
    h.set_ui([&] {
        ui::next_width(120.0f);
        next_width = ui::next_item_width();
        {
            ui::Group group;
            ui::button("Apply");
            h.mark("apply");
            ui::same_line();
            ui::button("Revert");
            h.mark("revert");
        }
        group_min = ui::item_min();
        group_max = ui::item_max();
    });
    h.step(2);
    CHECK(next_width == 120.0f);
    CHECK(group_min == h.rect("apply").min);
    CHECK(group_max == h.rect("revert").max);
}

TEST_CASE("A group is active while a field in it is held, and no longer once it is let go", "[ui][kit]") {
    KitHarness h;
    float inside = 1.0f;
    float outside = 1.0f;
    bool active = false;
    h.set_ui([&] {
        {
            const ui::Group group;
            ui::button("Before");
            (void)ui::drag("##inside", inside);
            h.mark("inside");
        }
        active = ui::item_active();
        (void)ui::drag("##outside", outside);
        h.mark("outside");
    });
    h.step(2);
    CHECK_FALSE(active);

    h.move_to(centre(h, "inside"));
    h.button_down();
    h.move_to(centre(h, "inside") + glm::vec2{30.0f, 0.0f});
    CHECK(active);
    // On the frame it is let go the field still reads as held, as it does
    // on its own; the frame after, neither it nor the group is.
    h.button_up();
    h.step();
    CHECK_FALSE(active);

    // A field held after the group is not the group's.
    h.move_to(centre(h, "outside"));
    h.button_down();
    h.move_to(centre(h, "outside") + glm::vec2{30.0f, 0.0f});
    CHECK_FALSE(active);
    h.button_up();
}

TEST_CASE("Text is an item the size text_size measures", "[ui][kit]") {
    KitHarness h;
    glm::vec2 size{};
    glm::vec2 measured{};
    h.set_ui([&] {
        ui::text("Nothing to bake");
        size = ui::item_max() - ui::item_min();
        measured = ui::text_size("Nothing to bake");
        ui::text("In a colour", ui::Color{1.0f, 0.0f, 0.0f, 1.0f});
    });
    h.step(2);
    CHECK(measured.x > 0.0f);
    CHECK(size == measured);
    CHECK(ui::line_height() > 0.0f);
    CHECK(ui::frame_height() > ui::line_height());
}

TEST_CASE("Cut text is whole when it fits, else cut to its width and whole in the tooltip", "[ui][kit]") {
    KitHarness h;
    const std::string path = "~/.config/fjedit/a/folder/a/long/way/down/keys.vim";
    float whole = 0.0f;
    float measured = 0.0f;
    float cut = 0.0f;
    h.set_ui([&] {
        ui::cut_text(path, 10000.0f, ftk::theme::text_secondary());
        whole = (ui::item_max() - ui::item_min()).x;
        measured = ui::text_size(path).x;
        h.mark("whole");
        ui::cut_text(path, 120.0f, ftk::theme::text_secondary());
        cut = (ui::item_max() - ui::item_min()).x;
        h.mark("cut");
    });
    h.step(2);
    CHECK(whole == measured);
    CHECK(cut > 0.0f);
    CHECK(cut <= 120.0f);
    h.hover("whole");
    h.step(30);
    CHECK_FALSE(h.tooltip().has_value());
    h.hover("cut");
    h.step(30);
    CHECK(h.tooltip().has_value());
}

TEST_CASE("A button is as wide as button_width says", "[ui][kit]") {
    KitHarness h;
    float said = 0.0f;
    float said_again = 0.0f;
    h.set_ui([&] {
        said = ui::button_width("Open");
        (void)ui::button("Open");
        h.mark("open");
        said_again = ui::button_width("Open##again");
        (void)ui::button("Open##again");
        h.mark("again");
    });
    h.step(2);
    CHECK(said > 0.0f);
    CHECK(h.rect("open").width() == Catch::Approx(said));
    CHECK(said_again == Catch::Approx(said));
    CHECK(h.rect("again").width() == Catch::Approx(said));
}

TEST_CASE("A change's marked text is the size of its text without the marks", "[ui][kit]") {
    KitHarness h;
    const std::string change = "Mass \x01" "1.00 kg\x02 -> \x03" "2.50 kg\x04";
    glm::vec2 size{};
    glm::vec2 measured{};
    h.set_ui([&] {
        ui::marked_text(change, ui::Color{1.0f, 1.0f, 1.0f, 1.0f});
        size = ui::item_max() - ui::item_min();
        measured = ui::text_size("Mass 1.00 kg -> 2.50 kg");
    });
    h.step(2);
    CHECK(ui::unmarked(change) == "Mass 1.00 kg -> 2.50 kg");
    // Equal but for the rounding of where it was placed.
    CHECK(size.x == Catch::Approx(measured.x));
    CHECK(size.y == Catch::Approx(measured.y));
}

TEST_CASE("Text on a line with a field, or set level with one, sits where a field's text does", "[ui][kit]") {
    KitHarness h;
    glm::vec2 button{};
    glm::vec2 after_button{};
    glm::vec2 line{};
    glm::vec2 level{};
    glm::vec2 after_level{};
    glm::vec2 own_line{};
    glm::vec2 own_text{};
    h.set_ui([&] {
        (void)ui::button("Save");
        button = ui::item_min();
        ui::same_line();
        ui::text("after a button");
        after_button = ui::item_min();
        line = ui::cursor();
        ui::align_text_to_frame();
        ui::text("set level");
        level = ui::item_min();
        ui::same_line();
        ui::text("after it");
        after_level = ui::item_min();
        own_line = ui::cursor();
        ui::text("a line of its own");
        own_text = ui::item_min();
    });
    h.step(2);
    const float drop = ui::frame_padding().y;
    CHECK(after_button.y == button.y + drop);
    CHECK(level.y == line.y + drop);
    CHECK(after_level.y == level.y);
    CHECK(own_text.y == own_line.y);
}

TEST_CASE("A hit area answers to the buttons it is given", "[ui][kit]") {
    KitHarness h;
    int left = 0;
    int either = 0;
    bool hovered = false;
    bool right_clicked = false;
    h.set_ui([&] {
        if (ui::hit_area("##left", {100.0f, 40.0f})) ++left;
        h.mark("left");
        if (ui::hit_area("##either", {100.0f, 40.0f}, {.right = true})) ++either;
        hovered = ui::item_hovered();
        right_clicked = right_clicked || ui::item_clicked(ui::MouseButton::Right);
        h.mark("either");
    });
    h.step(2);
    h.click("left", ui::MouseButton::Right);
    CHECK(left == 0);
    h.click("left");
    CHECK(left == 1);
    h.click("either", ui::MouseButton::Right);
    CHECK(either == 1);
    CHECK(hovered);
    CHECK(right_clicked);
}

TEST_CASE("A key is pressed once and repeated while it is held", "[ui][kit]") {
    KitHarness h;
    int pressed = 0;
    int repeated = 0;
    bool ctrl = false;
    h.set_ui([&] {
        if (ui::key_pressed(ui::Key::Delete)) ++pressed;
        if (ui::key_repeated(ui::Key::Delete)) ++repeated;
        ctrl = ui::modifiers().ctrl;
    });
    h.step();
    h.hold({.ctrl = true});
    h.key_down(ui::Key::Delete);
    h.step(58);  // a second held
    CHECK(pressed == 1);
    CHECK(repeated > 1);
    CHECK(ctrl);
    h.key_up(ui::Key::Delete);
    h.hold({});
    h.step();
    CHECK_FALSE(ctrl);
}

TEST_CASE("What is put on the clipboard is what it holds", "[ui][kit]") {
    KitHarness h;
    std::string held;
    h.set_ui([&] {
        ui::set_clipboard("[info] Opened harbour");
        held = ui::clipboard();
    });
    h.step();
    CHECK(held == "[info] Opened harbour");
}

TEST_CASE("An image is an item the size it is given", "[ui][kit]") {
    KitHarness h;
    glm::vec2 size{};
    h.set_ui([&] {
        ui::image(ui::Texture{0x1234}, {64.0f, 32.0f});
        size = ui::item_max() - ui::item_min();
    });
    h.step(2);
    CHECK(size == glm::vec2{64.0f, 32.0f});
}

TEST_CASE("What is stored under an id comes back the next frame", "[ui][kit]") {
    KitHarness h;
    int frame = 0;
    int seen = -2;
    h.set_ui([&] {
        const ui::Id picked = ui::id("##picked");
        seen = ui::stored_int(picked, -1);
        ui::store_int(picked, frame++);
        ui::store_bool(ui::id("##open"), true);
        ui::store_float(ui::id("##width"), 120.0f);
    });
    h.step();
    CHECK(seen == -1);
    h.step();
    CHECK(seen == 0);
    h.set_ui([&] {
        CHECK(ui::stored_bool(ui::id("##open")));
        CHECK(ui::stored_float(ui::id("##width")) == 120.0f);
        CHECK_FALSE(ui::stored_bool(ui::id("##never")));
    });
    h.step();
}

TEST_CASE("A font scope sets the size of the text drawn in it, and a colour scope its colour", "[ui][kit]") {
    KitHarness h;
    float inside = 0.0f;
    float after = 0.0f;
    float before = 0.0f;
    ui::Color text_inside{};
    h.set_ui([&] {
        before = ui::font_size();
        {
            ui::FontScope small({}, 9.0f);
            inside = ui::font_size();
            {
                ui::FontScope same({}, 0.0f);
                CHECK(ui::font_size() == 9.0f);
            }
            ui::TextColour dim(ftk::theme::text_secondary());
            text_inside = ui::text_colour();
        }
        after = ui::font_size();
    });
    h.step(2);
    CHECK(inside == 9.0f);
    CHECK(after == before);
    CHECK(text_inside == ftk::theme::text_secondary());
}

TEST_CASE("The mono face keeps each character to a cell, ligatures too", "[ui][kit]") {
    KitHarness h;
    float cell = 0.0f;
    float arrow = 0.0f;
    float equals = 0.0f;
    h.set_ui([&] {
        const ui::FontScope mono(ftk::theme::mono_font(), ftk::theme::SMALL_TEXT);
        cell = ui::text_size("a").x;
        arrow = ui::text_size("a->b").x;
        equals = ui::text_size("===").x;
    });
    h.step(2);
    REQUIRE(cell > 0.0f);
    CHECK(arrow == Catch::Approx(4.0f * cell));
    CHECK(equals == Catch::Approx(3.0f * cell));
}

TEST_CASE("The window being drawn knows where it is and whether the mouse is on it", "[ui][kit]") {
    KitHarness h;
    glm::vec2 pos{-1.0f};
    glm::vec2 size{};
    glm::vec2 content_min{};
    glm::vec2 padding{};
    bool hovered = false;
    h.set_ui([&] {
        pos = ui::window_pos();
        size = ui::window_size();
        content_min = ui::window_content_min();
        padding = ui::window_padding();
        hovered = ui::window_hovered();
        ui::button("Bake");
        h.mark("bake");
    });
    h.step(2);
    CHECK(pos == glm::vec2{0.0f, 0.0f});
    CHECK(size == glm::vec2{800.0f, 600.0f});
    CHECK(content_min == padding);
    h.hover("bake");
    CHECK(hovered);
}

TEST_CASE("A window scrolls to the cursor's line", "[ui][kit]") {
    KitHarness h;
    float scrolled = -1.0f;
    float most = -1.0f;
    h.set_ui([&] {
        for (int i = 0; i < 200; ++i) ui::text("A line of the log");
        ui::scroll_to_cursor(1.0f);
        scrolled = ui::scroll_y();
        most = ui::scroll_max_y();
    });
    h.step(3);
    CHECK(most > 0.0f);
    CHECK(scrolled == most);
}

TEST_CASE("A host window goes where it is placed and ends with end_host_window()", "[ui][kit]") {
    KitHarness h;
    glm::vec2 pos{};
    glm::vec2 size{};
    h.set_ui([&] {
        ui::next_window_place({40.0f, 30.0f}, {300.0f, 200.0f});
        if (ui::begin_host_window("##host")) {
            pos = ui::window_pos();
            size = ui::window_size();
        }
        ui::end_host_window();
    });
    h.step(2);
    CHECK(pos == glm::vec2{40.0f, 30.0f});
    CHECK(size == glm::vec2{300.0f, 200.0f});
}

TEST_CASE("An item that allows overlap lets one drawn over it take the mouse", "[ui][kit]") {
    KitHarness h;
    int under = 0;
    int over = 0;
    h.set_ui([&] {
        const glm::vec2 at = ui::cursor();
        ui::next_item_allows_overlap();
        if (ui::hit_area("##picture", {200.0f, 100.0f})) ++under;
        ui::set_cursor(at);
        if (ui::button("Fit")) ++over;
        h.mark("fit");
    });
    h.step(2);
    h.click("fit");
    CHECK(over == 1);
    CHECK(under == 0);
}

TEST_CASE("A selectable is clicked and lit while selected", "[ui][kit]") {
    KitHarness h;
    int picked = -1;
    h.set_ui([&] {
        for (int i = 0; i < 3; ++i) {
            ui::IdScope id(i);
            if (ui::selectable("Entry", picked == i)) picked = i;
            h.mark("entry" + std::to_string(i));
        }
    });
    h.step(2);
    h.click("entry1");
    CHECK(picked == 1);
    h.click("entry2");
    CHECK(picked == 2);
}

TEST_CASE("A selectable given its text shows all of it, ## and all, and is known by its label", "[ui][kit]") {
    KitHarness h;
    int picked = -1;
    float cut = 0.0f;
    h.set_ui([&] {
        cut = ui::text_size("opened ").x;
        for (int i = 0; i < 2; ++i) {
            ui::IdScope id(i);
            if (ui::selectable("##line", picked == i, {.text = "opened ##scene"})) picked = i;
            h.mark("line" + std::to_string(i));
        }
    });
    h.step(2);
    REQUIRE(h.find("opened ##scene", ftk::ui::core::Role::ListItem) != nullptr);
    CHECK(h.rect("line0").width() > cut);
    h.click("line1");
    CHECK(picked == 1);
}

TEST_CASE("A region is its own size and scrolls with the wheel unless told not to", "[ui][kit]") {
    KitHarness h;
    glm::vec2 inside{};
    float log_scroll = -1.0f;
    float lanes_scroll = -1.0f;
    h.set_ui([&] {
        if (auto log = ui::Region("##log", {200.0f, 100.0f})) {
            inside = ui::window_size();
            for (int i = 0; i < 50; ++i) ui::text("A line of the log");
            log_scroll = ui::scroll_y();
        }
        h.mark("log");
        if (auto lanes = ui::Region("##lanes", {200.0f, 100.0f}, {.wheel_scrolls = false})) {
            for (int i = 0; i < 50; ++i) ui::text("A lane");
            lanes_scroll = ui::scroll_y();
        }
        h.mark("lanes");
    });
    h.step(2);
    CHECK(inside == glm::vec2{200.0f, 100.0f});
    h.wheel(centre(h, "log"), -3.0f);
    h.step(2);
    CHECK(log_scroll > 0.0f);
    h.wheel(centre(h, "lanes"), -3.0f);
    h.step(2);
    CHECK(lanes_scroll == 0.0f);
}

TEST_CASE("The mouse pointer takes the shape asked for this frame", "[ui][kit]") {
    KitHarness h;
    h.set_ui([&] { ui::set_mouse_cursor(ui::MouseCursor::Text); });
    h.step();
    CHECK(h.cursor() == ui::MouseCursor::Text);
}

TEST_CASE("A drag carries its payload to a target that takes its type", "[ui][kit]") {
    KitHarness h;
    std::string dropped;
    std::string dropped_on_window;
    bool offered = false;
    bool offered_other = false;
    int quiet_frames = 0;
    int quiet_deliveries = 0;
    h.set_ui([&] {
        ui::button("Source");
        h.mark("source");
        if (auto drag = ui::DragSource()) {
            drag.carry("ASSET_PATH", std::string_view("textures/rock.png"));
            ui::text("rock.png");
        }
        ui::button("Target");
        h.mark("target");
        if (auto drop = ui::DropTarget()) {
            offered = offered || drop.offered("ASSET_PATH").has_value();
            offered_other = offered_other || drop.offered("HIERARCHY_NODE").has_value();
            if (auto payload = drop.dropped("ASSET_PATH")) dropped = payload->text();
        }
        ui::button("Quiet");
        h.mark("quiet");
        if (auto drop = ui::DropTarget()) {
            if (auto taking = drop.take("ASSET_PATH")) {
                ++quiet_frames;
                if (taking->delivered) ++quiet_deliveries;
            }
        }
        if (auto region = ui::Region("##panel", {200.0f, 100.0f})) {
            ui::text("Drop anywhere here");
            if (auto drop = ui::DropTarget::window("##panel_drop")) {
                if (auto payload = drop.dropped("ASSET_PATH")) dropped_on_window = payload->text();
            }
        }
        h.mark("panel");
    });
    h.step(2);
    h.drag(centre(h, "source"), centre(h, "target"));
    h.step();
    CHECK(offered);
    CHECK_FALSE(offered_other);
    CHECK(dropped == "textures/rock.png");

    h.drag(centre(h, "source"), centre(h, "quiet"));
    h.step();
    CHECK(quiet_frames > 1);
    CHECK(quiet_deliveries == 1);

    // Low in the region, under its only line.
    h.drag(centre(h, "source"), {centre(h, "panel").x, h.rect("panel").max.y - 10.0f});
    h.step();
    CHECK(dropped_on_window == "textures/rock.png");
}

TEST_CASE("Esc ends a drag with nothing dropped, and no other starts until the button is let go", "[ui][kit]") {
    KitHarness h;
    std::string dropped;
    bool offered = false;
    bool previewed = false;
    h.set_ui([&] {
        offered = false;
        previewed = false;
        ui::button("Source");
        h.mark("source");
        if (auto drag = ui::DragSource()) {
            previewed = true;
            drag.carry("ASSET_PATH", std::string_view("textures/rock.png"));
            ui::text("rock.png");
        }
        ui::button("Target");
        h.mark("target");
        if (auto drop = ui::DropTarget()) {
            offered = drop.offered("ASSET_PATH").has_value();
            if (auto payload = drop.dropped("ASSET_PATH")) dropped = payload->text();
        }
    });
    h.step(2);
    const glm::vec2 source = centre(h, "source");
    const glm::vec2 target = centre(h, "target");
    h.move_to(source);
    h.button_down();
    h.move_to(source + glm::vec2{20.0f, 0.0f});
    h.move_to(target);
    h.move_to(target + glm::vec2{1.0f, 0.0f});
    REQUIRE(previewed);
    REQUIRE(offered);

    h.press(ui::Key::Escape);
    CHECK_FALSE(previewed);
    CHECK_FALSE(offered);
    // Still held and moving: its source starts no new drag.
    h.move_to(target + glm::vec2{3.0f, 0.0f});
    h.move_to(target);
    CHECK_FALSE(previewed);
    CHECK_FALSE(offered);
    h.button_up();
    h.step();
    CHECK(dropped.empty());

    // The next drag carries as any does.
    h.drag(source, target);
    h.step();
    CHECK(dropped == "textures/rock.png");
}

TEST_CASE("A payload reads as text without the zero that ends it", "[ui][kit]") {
    const char with_zero[] = "rock.png";
    CHECK(ui::Payload{with_zero, sizeof(with_zero)}.text() == "rock.png");
    CHECK(ui::Payload{with_zero, 4}.text() == "rock");
    CHECK(ui::Payload{}.text().empty());
}

TEST_CASE("Under the mouse is hovered even while another item is held", "[ui][kit]") {
    KitHarness h;
    bool hovered = false;
    bool under = false;
    bool window_hovered = false;
    bool window_under = false;
    bool over = false;
    glm::vec2 field_min{};
    h.set_ui([&] {
        ui::button("Held");
        h.mark("held");
        ui::button("Canvas");
        h.mark("canvas");
        hovered = ui::item_hovered();
        under = ui::item_under_mouse();
        window_hovered = ui::window_hovered();
        window_under = ui::window_under_mouse();
        field_min = ui::item_min();
        over = ui::mouse_over(ui::item_min(), ui::item_max());
    });
    h.step(2);
    h.move_to(centre(h, "held"));
    h.button_down();
    CHECK_FALSE(over);
    h.move_to(centre(h, "canvas"));
    h.step();
    CHECK(over);
    CHECK_FALSE(hovered);
    CHECK(under);
    CHECK_FALSE(window_hovered);
    CHECK(window_under);
    h.button_up();
    h.step();
    CHECK(hovered);
    CHECK(window_hovered);
}

TEST_CASE("A window placed each frame stays put and keeps within its size limits", "[ui][kit]") {
    KitHarness h;
    glm::vec2 pos{};
    glm::vec2 size{};
    ui::WorkArea area{};
    h.set_ui([&] {
        area = ui::work_area();
        ui::next_window_size_once({300.0f, 50.0f});
        ui::next_window_size_limits({200.0f, 100.0f}, {400.0f, 150.0f});
        ui::next_window_pos({40.0f, 60.0f});
        if (auto panel = ui::Panel("", "Pinned", {.remembered = false})) {
            pos = ui::window_pos();
            size = ui::window_size();
        }
    });
    h.step(3);
    CHECK(area.size == h.size());
    CHECK(pos == glm::vec2{40.0f, 60.0f});
    CHECK(size == glm::vec2{300.0f, 100.0f});
}

TEST_CASE("The windows shown are the top-level ones, found again by name", "[ui][kit]") {
    KitHarness h;
    std::vector<ui::ShownWindow> shown;
    h.set_ui([&] {
        ui::next_window_place({100.0f, 120.0f}, {240.0f, 160.0f});
        if (auto panel = ui::Panel("", "Stats", {.remembered = false})) {
            if (auto region = ui::Region("##inside", {50.0f, 50.0f})) ui::text("a line");
        }
        shown = ui::shown_windows();
    });
    h.step(2);
    std::vector<std::string> names;
    for (const auto& window : shown) names.push_back(window.name);
    CHECK(std::find(names.begin(), names.end(), "Stats") != names.end());
    CHECK(std::find(names.begin(), names.end(), "##harness") != names.end());
    for (const auto& name : names) CHECK(name.find("##inside") == std::string::npos);

    const auto stats = ui::window_named("Stats");
    REQUIRE(stats.has_value());
    CHECK(stats->pos == glm::vec2{100.0f, 120.0f});
    CHECK(stats->size == glm::vec2{240.0f, 160.0f});
    CHECK_FALSE(ui::window_named("Nowhere").has_value());
}

TEST_CASE("Drawing in a window again lands in it, and a window is given focus", "[ui][kit]") {
    KitHarness h;
    glm::vec2 panel_pos{};
    glm::vec2 again_pos{};
    glm::vec2 note_min{};
    bool focused = false;
    bool focus_viewport = false;
    h.set_ui([&] {
        ui::next_window_place({300.0f, 100.0f}, {200.0f, 150.0f});
        if (auto panel = ui::Panel("", "Viewport", {.remembered = false})) panel_pos = ui::window_pos();
        ui::next_window_place({50.0f, 100.0f}, {200.0f, 150.0f});
        if (auto panel = ui::Panel("", "Hierarchy", {.remembered = false})) focused = ui::window_focused();
        if (focus_viewport) ui::focus_window("Viewport");
        // Back in the viewport, after another window was drawn.
        if (auto again = ui::InWindow("Viewport")) {
            again_pos = ui::window_pos();
            ui::text("Over the picture");
            note_min = ui::item_min();
        }
    });
    h.step(3);
    CHECK(again_pos == panel_pos);
    CHECK(note_min.x >= 300.0f);
    CHECK(note_min.x < 500.0f);
    CHECK(focused);
    focus_viewport = true;
    h.step(2);
    CHECK_FALSE(focused);
}

TEST_CASE("A window shown again comes forward over the ones that overlap it", "[ui][kit]") {
    KitHarness h;
    bool show_browser = true;
    bool over_browser = false;
    bool over_graph = false;
    h.set_ui([&] {
        if (show_browser) {
            ui::next_window_place({100.0f, 100.0f}, {300.0f, 200.0f});
            if (auto panel = ui::Panel("", "Browser", {.remembered = false})) {
                (void)ui::hit_area("##browser", {280.0f, 160.0f});
                over_browser = ui::item_hovered();
            }
        }
        ui::next_window_place({200.0f, 150.0f}, {300.0f, 200.0f});
        if (auto panel = ui::Panel("", "Graph", {.remembered = false})) {
            (void)ui::hit_area("##graph", {280.0f, 160.0f});
            over_graph = ui::item_hovered();
        }
    });
    // The one opened last is in front where they overlap.
    h.step(3);
    h.move_to({300.0f, 250.0f});
    h.step(2);
    CHECK(over_graph);
    CHECK_FALSE(over_browser);

    // Closed and opened again, the browser is.
    show_browser = false;
    h.step(2);
    show_browser = true;
    h.step(3);
    CHECK(over_browser);
    CHECK_FALSE(over_graph);
}

TEST_CASE("A claimed key is still read by its shortcut", "[ui][kit]") {
    KitHarness h;
    bool snapped = false;
    h.set_ui([&] {
        ui::claim_key(ui::Key::End, "##snap_to_floor");
        snapped = snapped || ui::key_pressed(ui::Key::End);
    });
    h.step(2);
    h.press(ui::Key::End);
    CHECK(snapped);
}

TEST_CASE("What is drawn in a disabled scope takes no clicks", "[ui][kit]") {
    KitHarness h;
    bool disabled = true;
    int clicks = 0;
    h.set_ui([&] {
        ui::Disabled off(disabled);
        if (ui::button("Bake")) ++clicks;
        h.mark("bake");
    });
    h.step(2);
    h.click("bake");
    CHECK(clicks == 0);

    disabled = false;
    h.step();
    h.click("bake");
    CHECK(clicks == 1);
}

TEST_CASE("An id scope tells the same label apart", "[ui][kit]") {
    KitHarness h;
    int first = 0;
    int second = 0;
    h.set_ui([&] {
        {
            ui::IdScope id("first");
            if (ui::button("Remove")) ++first;
            h.mark("first");
        }
        {
            ui::IdScope id(2);
            if (ui::button("Remove")) ++second;
            h.mark("second");
        }
    });
    h.step(2);
    h.click("second");
    CHECK(first == 0);
    CHECK(second == 1);
}

TEST_CASE("The next item goes beside the last, or under a rule", "[ui][kit]") {
    KitHarness h;
    h.set_ui([&] {
        ui::button("Apply");
        h.mark("apply");
        ui::same_line();
        ui::button("Revert");
        h.mark("revert");
        ui::same_line(20.0f);
        ui::button("Close");
        h.mark("close");
        ui::separator();
        ui::button("Help");
        h.mark("help");
    });
    h.step(2);
    CHECK(h.rect("revert").min.y == h.rect("apply").min.y);
    CHECK(h.rect("revert").min.x > h.rect("apply").max.x);
    CHECK(h.rect("close").min.x == h.rect("revert").max.x + 20.0f);
    CHECK(h.rect("help").min.y > h.rect("apply").max.y);
}

TEST_CASE("An item's tooltip shows once the mouse rests on it", "[ui][kit]") {
    KitHarness h;
    h.set_ui([&] {
        ui::button("Bake");
        ui::item_tooltip("Bakes every probe in the scene", "Ctrl B");
        h.mark("bake");
        ui::button("Clear");
        h.mark("clear");
    });
    h.step(2);
    h.hover("clear");
    CHECK_FALSE(h.tooltip().has_value());
    h.hover("bake");
    CHECK(h.tooltip().has_value());
}

TEST_CASE("An item's tooltip can hold what is drawn in its scope", "[ui][kit]") {
    KitHarness h;
    bool drawn = false;
    glm::vec2 text_min{};
    h.set_ui([&] {
        ui::button("Crate");
        h.mark("crate");
        drawn = false;
        if (auto tip = ui::ItemTooltip()) {
            ui::text("props/crate_wood.png");
            text_min = ui::item_min();
            drawn = true;
        }
    });
    h.step(2);
    CHECK_FALSE(drawn);
    h.hover("crate");
    REQUIRE(drawn);
    const auto tooltip = h.tooltip();
    REQUIRE(tooltip.has_value());
    CHECK(tooltip->contains(text_min));
}

TEST_CASE("The item just drawn is the same after a tooltip shows for it", "[ui][kit]") {
    KitHarness h;
    bool hovered_after = false;
    glm::vec2 min_after{};
    h.set_ui([&] {
        ui::button("Bake");
        h.mark("bake");
        ui::item_tooltip("Bakes every probe in the scene");
        hovered_after = ui::item_hovered();
        min_after = ui::item_min();
    });
    h.step(2);
    h.hover("bake");
    REQUIRE(h.tooltip().has_value());
    CHECK(hovered_after);
    CHECK(min_after == h.rect("bake").min);
}

TEST_CASE("A notice strip says what wants an answer and gives the answer clicked", "[ui][kit]") {
    KitHarness h;
    std::optional<std::size_t> chosen;
    h.set_ui([&] {
        const char* answers[] = {"Keep mine", "Load from disk"};
        if (const auto answer = ui::notice_strip(ui::Severity::Warning, "toon.fjsl changed on disk", answers)) chosen = answer;
    });
    h.step(2);
    const auto* load = h.find("Load from disk", ftk::ui::core::Role::Button);
    REQUIRE(load != nullptr);
    h.click_at(load->rect.centre());
    CHECK(chosen == std::optional<std::size_t>{1});
}

TEST_CASE("A note line says what the fields under it stand for, on the raised surface", "[ui][kit]") {
    KitHarness h;
    float width = 400.0f;
    h.set_ui([&] {
        if (auto region = ui::Region("##note", {width, 80.0f})) {
            ui::note_line(ftk::ui::icon::checked, "Settings for the ", "179 picked");
            h.mark("note");
            ui::text("below");
            h.mark("below");
        }
    });
    h.step(2);
    const auto note = h.rect("note");
    CHECK(note.min.y < h.rect("below").min.y);
    CHECK(h.rect("below").min.y >= note.max.y);
    const uint32_t raised = ftk::ui::core::pack_colour(ftk::theme::surface_raised());
    const uint32_t strong = ftk::ui::core::pack_colour(ftk::theme::text());
    const uint32_t quiet = ftk::ui::core::pack_colour(ftk::theme::text_secondary());
    // Drawn inside the note line, in `colour`.
    const auto drew = [&](uint32_t colour) {
        const auto note_rect = h.rect("note");
        for (const auto& root : h.output().roots) {
            for (const auto& p : root.draw->primitives) {
                if (p.colour == colour && note_rect.contains((p.min + p.max) * 0.5f)) return true;
            }
        }
        return false;
    };
    CHECK(drew(raised));
    CHECK(drew(strong));
    CHECK(drew(quiet));

    // Too narrow for both: one line, cut, in the quiet colour alone.
    width = 90.0f;
    h.step(2);
    CHECK(drew(raised));
    CHECK_FALSE(drew(strong));
    CHECK(h.rect("below").min.y >= h.rect("note").max.y);
}

TEST_CASE("An outcome line says what entering a field will do, its names in the text colour, cut to its width",
          "[ui][kit]") {
    KitHarness h;
    float width = 400.0f;
    std::optional<ui::Severity> severity;
    h.set_ui([&] {
        if (auto region = ui::Region("##outcome", {width, 80.0f})) {
            ui::outcome_line(severity, "Replace every \x01gust\x02 with \x01wind\x02 on line 44");
            h.mark("line");
            ui::text("below");
            h.mark("below");
        }
    });
    h.step(2);
    CHECK(h.rect("below").min.y >= h.rect("line").max.y);
    // Drawn inside the line in `colour`; and the right edge of all it drew.
    const auto drew = [&](ftk::ui::Color colour) {
        const uint32_t packed = ftk::ui::core::pack_colour(colour);
        for (const auto& root : h.output().roots) {
            for (const auto& p : root.draw->primitives) {
                if (p.colour == packed && h.rect("line").contains((p.min + p.max) * 0.5f)) return true;
            }
        }
        return false;
    };
    const auto right_edge = [&] {
        float right = 0.0f;
        for (const auto& root : h.output().roots) {
            for (const auto& p : root.draw->primitives) {
                if (h.rect("line").contains((p.min + p.max) * 0.5f)) right = std::max(right, p.max.x);
            }
        }
        return right;
    };
    CHECK(drew(ftk::theme::text_secondary()));
    CHECK(drew(ftk::theme::text()));

    // What will be refused: the words in the warning colour, the names as
    // they were.
    severity = ui::Severity::Warning;
    h.step(2);
    CHECK(drew(ftk::theme::warning()));
    CHECK(drew(ftk::theme::text()));
    CHECK_FALSE(drew(ftk::theme::text_secondary()));

    // Too narrow for it all: one line, cut short of the edge.
    severity.reset();
    width = 120.0f;
    h.step(2);
    CHECK(h.rect("below").min.y >= h.rect("line").max.y);
    CHECK(right_edge() <= h.rect("line").max.x + 0.5f);
}

TEST_CASE("A region that resizes keeps the width its right edge is dragged to", "[ui][kit]") {
    KitHarness h;
    h.set_ui([&] {
        if (auto left = ui::Region("##left", {200.0f, 300.0f}, {.resize_x = true})) {}
        h.mark("left");
        ui::same_line(0.0f);
        if (auto right = ui::Region("##right", {0.0f, 300.0f})) {}
    });
    h.step(2);
    const auto left = h.rect("left");
    REQUIRE(left.width() == 200.0f);
    h.drag({left.max.x, left.centre().y}, {left.max.x + 80.0f, left.centre().y});
    h.step(2);
    CHECK(h.rect("left").width() == 280.0f);
    // Not narrower than a handful of pixels.
    h.drag({h.rect("left").max.x, left.centre().y}, {left.min.x - 100.0f, left.centre().y});
    h.step(2);
    CHECK(h.rect("left").width() == 60.0f);
}
