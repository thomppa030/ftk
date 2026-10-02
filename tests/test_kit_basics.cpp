#include "ftk/test/imgui_harness.hpp"
#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/image.hpp"
#include "ftk/ui/kit/input.hpp"
#include "ftk/ui/kit/item.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/menu.hpp"
#include "ftk/ui/kit/popup.hpp"
#include "ftk/ui/kit/text.hpp"

#include <catch2/catch_test_macros.hpp>
#include <imgui.h>

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
