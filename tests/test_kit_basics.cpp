#include "ftk/test/imgui_harness.hpp"
#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/choice.hpp"
#include "ftk/ui/kit/color_field.hpp"
#include "ftk/ui/kit/drag_drop.hpp"
#include "ftk/ui/kit/image.hpp"
#include "ftk/ui/kit/input.hpp"
#include "ftk/ui/kit/item.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/loading.hpp"
#include "ftk/ui/kit/menu.hpp"
#include "ftk/ui/kit/panel.hpp"
#include "ftk/ui/kit/plot.hpp"
#include "ftk/ui/kit/popup.hpp"
#include "ftk/ui/kit/region.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/selectable.hpp"
#include "ftk/ui/kit/state.hpp"
#include "ftk/ui/kit/table.hpp"
#include "ftk/ui/kit/text_field.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/viewport_toolbar.hpp"
#include "ftk/ui/kit/window.hpp"
#include "ftk/ui/theme.hpp"

#include <catch2/catch_test_macros.hpp>
#include <imgui.h>

#include <array>
#include <string>
#include <utility>
#include <vector>

using ftk::test::ImGuiHarness;
namespace ui = ftk::ui;

namespace {

// Presses and releases `button` at `at`, a frame for each, the way a click
// arrives.
void click_with(ImGuiHarness& h, glm::vec2 at, ImGuiMouseButton button) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(at.x, at.y);
    h.step();
    io.AddMouseButtonEvent(button, true);
    h.step();
    io.AddMouseButtonEvent(button, false);
    h.step();
}

glm::vec2 centre(const ImGuiHarness& h, const std::string& name) {
    return (glm::vec2(h.rect_min(name)) + glm::vec2(h.rect_max(name))) * 0.5f;
}

} // namespace

TEST_CASE("The cursor is where the next item goes, and reserve takes room there", "[ui][kit]") {
    ImGuiHarness h;
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

TEST_CASE("next_width sets the next field's width, and a group is one item", "[ui][kit]") {
    ImGuiHarness h;
    float field_width = 0.0f;
    glm::vec2 group_min{};
    glm::vec2 group_max{};
    h.set_ui([&] {
        static float value = 0.0f;
        ui::next_width(120.0f);
        CHECK(ui::next_item_width() == 120.0f);
        ImGui::SliderFloat("##value", &value, 0.0f, 1.0f);
        field_width = ui::item_max().x - ui::item_min().x;
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
    CHECK(field_width == 120.0f);
    CHECK(group_min == glm::vec2(h.rect_min("apply")));
    CHECK(group_max == glm::vec2(h.rect_max("revert")));
}

TEST_CASE("Text is an item the size text_size measures", "[ui][kit]") {
    ImGuiHarness h;
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

TEST_CASE("A hit area answers to the buttons it is given", "[ui][kit]") {
    ImGuiHarness h;
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
    click_with(h, centre(h, "left"), ImGuiMouseButton_Right);
    CHECK(left == 0);
    click_with(h, centre(h, "left"), ImGuiMouseButton_Left);
    CHECK(left == 1);
    click_with(h, centre(h, "either"), ImGuiMouseButton_Right);
    CHECK(either == 1);
    CHECK(hovered);
    CHECK(right_clicked);
}

TEST_CASE("A key is pressed once and repeated while it is held", "[ui][kit]") {
    ImGuiHarness h;
    int pressed = 0;
    int repeated = 0;
    bool ctrl = false;
    h.set_ui([&] {
        if (ui::key_pressed(ui::Key::Delete)) ++pressed;
        if (ui::key_repeated(ui::Key::Delete)) ++repeated;
        ctrl = ui::modifiers().ctrl;
    });
    h.step();
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_Delete, true);
    h.step(60);  // a second held
    CHECK(pressed == 1);
    CHECK(repeated > 1);
    CHECK(ctrl);
    io.AddKeyEvent(ImGuiKey_Delete, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    h.step(3);
    CHECK_FALSE(ctrl);
}

TEST_CASE("What is put on the clipboard is what it holds", "[ui][kit]") {
    ImGuiHarness h;
    std::string held;
    h.set_ui([&] {
        ui::set_clipboard("[info] Opened harbour");
        held = ui::clipboard();
    });
    h.step();
    CHECK(held == "[info] Opened harbour");
}

TEST_CASE("A popup opens by name and closes when an entry is chosen", "[ui][kit]") {
    ImGuiHarness h;
    int chosen = 0;
    bool open = false;
    h.set_ui([&] {
        if (ui::button("Menu")) ui::open_popup("##menu");
        h.mark("menu");
        if (auto menu = ui::Popup("##menu")) {
            if (ui::menu_item({.label = "Rename"})) ++chosen;
            h.mark("rename");
        }
        open = ui::popup_open("##menu");
    });
    h.step(2);
    CHECK_FALSE(open);
    h.click("menu");
    h.step();
    CHECK(open);
    h.click("rename");
    h.step();
    CHECK(chosen == 1);
    CHECK_FALSE(open);
}

TEST_CASE("A context menu opens on a right click on its item", "[ui][kit]") {
    ImGuiHarness h;
    bool shown = false;
    h.set_ui([&] {
        ui::button("Track");
        h.mark("track");
        shown = false;
        if (auto menu = ui::ContextMenu("##track")) {
            shown = true;
            ui::menu_item({.label = "Delete track", .destructive = true});
        }
    });
    h.step(2);
    CHECK_FALSE(shown);
    click_with(h, centre(h, "track"), ImGuiMouseButton_Right);
    h.step();
    CHECK(shown);
}

TEST_CASE("An image is an item the size it is given", "[ui][kit]") {
    ImGuiHarness h;
    glm::vec2 size{};
    h.set_ui([&] {
        ui::image(ui::Texture{0x1234}, {64.0f, 32.0f});
        size = ui::item_max() - ui::item_min();
    });
    h.step(2);
    CHECK(size == glm::vec2{64.0f, 32.0f});
}

TEST_CASE("What is stored under an id comes back the next frame", "[ui][kit]") {
    ImGuiHarness h;
    int frame = 0;
    int seen = -2;
    bool same_id = false;
    h.set_ui([&] {
        const ui::Id picked = ui::id("##picked");
        same_id = picked == ImGui::GetID("##picked");
        seen = ui::stored_int(picked, -1);
        ui::store_int(picked, frame++);
        ui::store_bool(ui::id("##open"), true);
        ui::store_float(ui::id("##width"), 120.0f);
    });
    h.step();
    CHECK(seen == -1);
    h.step();
    CHECK(seen == 0);
    CHECK(same_id);
    h.set_ui([&] {
        CHECK(ui::stored_bool(ui::id("##open")));
        CHECK(ui::stored_float(ui::id("##width")) == 120.0f);
        CHECK_FALSE(ui::stored_bool(ui::id("##never")));
    });
    h.step();
}

TEST_CASE("A font scope sets the size of the text drawn in it, and a colour scope its colour", "[ui][kit]") {
    ImGuiHarness h;
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
            text_inside = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        }
        after = ui::font_size();
    });
    h.step(2);
    CHECK(inside == 9.0f);
    CHECK(after == before);
    CHECK(text_inside == ftk::theme::text_secondary());
}

TEST_CASE("The style's spacing and padding are the kit's metrics", "[ui][kit]") {
    ImGuiHarness h;
    h.set_ui([&] {
        CHECK(ui::item_spacing() == glm::vec2(ImGui::GetStyle().ItemSpacing));
        CHECK(ui::item_inner_spacing() == glm::vec2(ImGui::GetStyle().ItemInnerSpacing));
        CHECK(ui::frame_padding() == glm::vec2(ImGui::GetStyle().FramePadding));
    });
    h.step();
}

TEST_CASE("The window being drawn knows where it is and whether the mouse is on it", "[ui][kit]") {
    ImGuiHarness h;
    glm::vec2 pos{-1.0f};
    glm::vec2 size{};
    glm::vec2 content_min{};
    bool hovered = false;
    h.set_ui([&] {
        pos = ui::window_pos();
        size = ui::window_size();
        content_min = ui::window_content_min();
        hovered = ui::window_hovered();
        ui::button("Bake");
        h.mark("bake");
    });
    h.step(2);
    CHECK(pos == glm::vec2{0.0f, 0.0f});
    CHECK(size == glm::vec2{800.0f, 600.0f});
    CHECK(content_min == glm::vec2(ImGui::GetStyle().WindowPadding));
    h.hover("bake");
    CHECK(hovered);
}

TEST_CASE("A window scrolls to the cursor's line", "[ui][kit]") {
    ImGuiHarness h;
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
    ImGuiHarness h;
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

TEST_CASE("A window's first size is only its first", "[ui][kit]") {
    ImGuiHarness h;
    glm::vec2 size{};
    h.set_ui([&] {
        ui::next_window_size_once({320.0f, 240.0f});
        ImGui::Begin("Preview");
        size = ui::window_size();
        ImGui::End();
    });
    h.step(2);
    CHECK(size == glm::vec2{320.0f, 240.0f});
    ImGui::SetWindowSize("Preview", {400.0f, 300.0f});
    h.step(2);
    CHECK(size == glm::vec2{400.0f, 300.0f});
}

TEST_CASE("An item that allows overlap lets one drawn over it take the mouse", "[ui][kit]") {
    ImGuiHarness h;
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
    ImGuiHarness h;
    int picked = -1;
    h.set_ui([&] {
        for (int i = 0; i < 3; ++i) {
            ui::IdScope id(i);
            if (ui::selectable("Entry", picked == i)) picked = i;
            h.mark("entry" + std::to_string(i));
        }
        ui::progress_bar(0.5f, 120.0f, "Halfway");
        CHECK(ui::item_max().x - ui::item_min().x == 120.0f);
    });
    h.step(2);
    h.click("entry1");
    CHECK(picked == 1);
    h.click("entry2");
    CHECK(picked == 2);
}

TEST_CASE("A region is its own size and scrolls with the wheel unless told not to", "[ui][kit]") {
    ImGuiHarness h;
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

TEST_CASE("A table's rows go under its headers, and a click on a header sorts by it", "[ui][kit]") {
    ImGuiHarness h;
    static constexpr ui::Column COLUMNS[] = {
        {.name = "Name", .default_sort = true},
        {.name = "Size", .sizing = ui::ColumnWidth::Fixed, .width = 80.0f},
    };
    std::vector<ui::TableSort> sorts;
    h.set_ui([&] {
        if (auto table = ui::Table("##files", COLUMNS, {.sortable = true, .scroll = true})) {
            ui::table_headers();
            if (auto sort = ui::table_sort_changed()) sorts.push_back(*sort);
            for (int i = 0; i < 3; ++i) {
                ui::IdScope row(i);
                ui::table_next_row();
                ui::table_next_column();
                ui::text("terrain.fjmesh");
                h.mark("name" + std::to_string(i));
                ui::table_next_column();
                ui::text("12 KB");
                h.mark("size" + std::to_string(i));
            }
        }
    });
    h.step(2);
    REQUIRE(sorts.size() == 1);
    CHECK(sorts[0].column == 0);
    CHECK(sorts[0].ascending);
    CHECK(h.rect_min("size0").x > h.rect_max("name0").x);
    CHECK(h.rect_min("name1").y > h.rect_min("name0").y);
    CHECK(h.rect_min("name1").x == h.rect_min("name0").x);

    // The headers are a row above the first; the size column's is clicked.
    const ImVec2 size_header{h.rect_min("size0").x + 10.0f, h.rect_min("size0").y - 8.0f};
    h.click_at(size_header);
    REQUIRE(sorts.size() == 2);
    CHECK(sorts[1].column == 1);
    CHECK(sorts[1].ascending);
    h.click_at(size_header);
    REQUIRE(sorts.size() == 3);
    CHECK(sorts[2].column == 1);
    CHECK_FALSE(sorts[2].ascending);
}

TEST_CASE("A plot is an item the size it is given", "[ui][kit]") {
    ImGuiHarness h;
    const std::array<float, 4> history{1.0f, 3.0f, 2.0f, 5.0f};
    glm::vec2 size{};
    h.set_ui([&] {
        ui::plot_lines("##frame", history, 1, {200.0f, 35.0f}, {.max = 33.3f});
        size = ui::item_max() - ui::item_min();
    });
    h.step(2);
    CHECK(size == glm::vec2{200.0f, 35.0f});
}

TEST_CASE("A drop-down's list is the caller's, and an entry chosen closes it", "[ui][kit]") {
    ImGuiHarness h;
    int picked = -1;
    bool any_open = false;
    float width = 0.0f;
    h.set_ui([&] {
        {
            ui::next_width(160.0f);
            ui::Dropdown list("##event", "Pick an event...");
            if (!list) {
                h.mark("dropdown");
                width = ui::item_max().x - ui::item_min().x;
            }
            if (list) {
                for (int i = 0; i < 2; ++i) {
                    ui::IdScope entry(i);
                    if (ui::selectable(i == 0 ? "Storm" : "Fog", false)) picked = i;
                    h.mark("entry" + std::to_string(i));
                }
            }
        }
        any_open = ui::any_popup_open();
    });
    h.step(2);
    CHECK(width == 160.0f);
    CHECK_FALSE(any_open);
    h.click("dropdown");
    h.step();
    CHECK(any_open);
    h.click("entry1");
    h.step();
    CHECK(picked == 1);
    CHECK_FALSE(any_open);
}

TEST_CASE("A menu on the bar opens on a click and runs its entries", "[ui][kit]") {
    ImGuiHarness h;
    int saved = 0;
    h.set_ui([&] {
        if (auto bar = ui::MenuBar()) {
            const bool open = ui::begin_bar_menu("File");
            if (!open) h.mark("file");
            if (open) {
                if (ui::menu_item({.label = "Save scene", .shortcut = "Ctrl S"})) ++saved;
                h.mark("save");
                ui::end_menu();
            }
        }
    });
    h.step(2);
    h.click("file");
    h.step();
    h.click("save");
    h.step();
    CHECK(saved == 1);
}

TEST_CASE("A small button is a line of text tall and is clicked", "[ui][kit]") {
    ImGuiHarness h;
    int stops = 0;
    float height = 0.0f;
    h.set_ui([&] {
        if (ui::small_button("Stop")) ++stops;
        h.mark("stop");
        height = ui::item_max().y - ui::item_min().y;
    });
    h.step(2);
    CHECK(height == ui::line_height());
    h.click("stop");
    CHECK(stops == 1);
}

TEST_CASE("A text entry's text is live, and Enter is told apart from leaving it", "[ui][kit]") {
    ImGuiHarness h;
    std::string name;
    std::string seen;
    int entered = 0;
    int left = 0;
    bool focus = false;
    h.set_ui([&] {
        if (ui::text_entry("##name", name, {.hint = "Project name", .focus = std::exchange(focus, false)})) ++entered;
        if (ui::item_deactivated()) ++left;
        h.mark("name");
        seen = name;
        ui::button("Elsewhere");
        h.mark("elsewhere");
    });
    h.step(2);
    // Focus asked for on one frame takes the keyboard on the next.
    focus = true;
    h.step(2);
    h.type("Fjord");
    h.step();
    CHECK(seen == "Fjord");
    CHECK(entered == 0);
    h.press(ImGuiKey_Enter);
    h.step();
    CHECK(entered == 1);

    h.click("name");
    h.type("s");
    h.step();
    h.click("elsewhere");
    CHECK(name == "Fjords");
    CHECK(entered == 1);
    CHECK(left >= 1);
}

TEST_CASE("The mouse pointer takes the shape asked for this frame", "[ui][kit]") {
    ImGuiHarness h;
    ImGuiMouseCursor shape = ImGuiMouseCursor_None;
    h.set_ui([&] {
        ui::set_mouse_cursor(ui::MouseCursor::Text);
        shape = ImGui::GetMouseCursor();
    });
    h.step();
    CHECK(shape == ImGuiMouseCursor_TextInput);
}

TEST_CASE("A drag carries its payload to a target that takes its type", "[ui][kit]") {
    ImGuiHarness h;
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
    h.drag(centre(h, "source"), {centre(h, "panel").x, h.rect_max("panel").y - 10.0f});
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
    ImGuiHarness h;
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
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(centre(h, "held").x, centre(h, "held").y);
    h.step();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    h.step();
    CHECK_FALSE(over);
    io.AddMousePosEvent(centre(h, "canvas").x, centre(h, "canvas").y);
    h.step(2);
    CHECK(over);
    CHECK_FALSE(hovered);
    CHECK(under);
    CHECK_FALSE(window_hovered);
    CHECK(window_under);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    h.step(2);
    CHECK(hovered);
    CHECK(window_hovered);
}

TEST_CASE("A window placed each frame stays put and keeps within its size limits", "[ui][kit]") {
    ImGuiHarness h;
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
    CHECK(area.size == glm::vec2(ImGui::GetIO().DisplaySize));
    CHECK(pos == glm::vec2{40.0f, 60.0f});
    CHECK(size == glm::vec2{300.0f, 100.0f});
}

TEST_CASE("A swatch is clicked, and the picker writes the colour it is dragged to", "[ui][kit]") {
    ImGuiHarness h;
    int picked = 0;
    glm::vec3 colour{0.2f, 0.4f, 0.6f};
    ui::Edit last{};
    bool committed = false;
    h.set_ui([&] {
        if (ui::swatch("##amber", {0.83f, 0.63f, 0.31f, 1.0f}, {20.0f, 20.0f})) ++picked;
        h.mark("swatch");
        ui::bullet_text("textures/rock.png");
        ui::next_width(200.0f);
        last = ui::color_picker("##custom", colour);
        committed = committed || last.committed;
        h.mark("picker");
    });
    h.step(2);
    CHECK(h.rect_max("swatch").x - h.rect_min("swatch").x == 20.0f);
    h.click("swatch");
    CHECK(picked == 1);
    // A press near the square's top-left corner: white, the brightest and
    // least saturated.
    const glm::vec2 corner = glm::vec2(h.rect_min("picker")) + glm::vec2{2.0f, 2.0f};
    h.click_at(corner);
    h.step();
    CHECK(colour.r > 0.9f);
    CHECK(colour.g > 0.9f);
    CHECK(colour.b > 0.9f);
    CHECK(committed);
}

TEST_CASE("A table knows which of its rows the mouse is on", "[ui][kit]") {
    ImGuiHarness h;
    static constexpr ui::Column COLUMNS[] = {{.name = "Property"}, {.name = "Value"}};
    std::array<bool, 2> hovered{};
    h.set_ui([&] {
        if (auto table = ui::Table("##rows", COLUMNS)) {
            for (int i = 0; i < 2; ++i) {
                ui::IdScope row(i);
                ui::table_next_row();
                ui::table_next_column();
                ui::text("width");
                h.mark("label" + std::to_string(i));
                ui::table_next_column();
                ui::text("120 px");
                hovered[static_cast<std::size_t>(i)] = ui::table_row_hovered();
            }
        }
    });
    h.step(2);
    h.hover("label1");
    CHECK_FALSE(hovered[0]);
    CHECK(hovered[1]);
}
