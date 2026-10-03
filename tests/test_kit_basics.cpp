#include "kit_harness.hpp"

#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/drag_drop.hpp"
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

#include <catch2/catch_test_macros.hpp>

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
