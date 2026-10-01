#include "ftk/test/imgui_harness.hpp"
#include "ftk/ui/kit/dialog.hpp"

#include <catch2/catch_test_macros.hpp>
#include <imgui_internal.h>

#include <string>
#include <vector>

using ftk::test::ImGuiHarness;
namespace ui = ftk::ui;

namespace {

// A name prompt opened on the first frame; `answer` keeps the last answer
// it gave.
struct Prompt {
    bool open_now{true};
    std::string name;
    ui::DialogAnswer answer{ui::DialogAnswer::None};
    bool open{false};

    void draw() {
        if (open_now) {
            ui::open_dialog("##prompt");
            open_now = false;
        }
        const auto given = ui::prompt_dialog("##prompt", {.title = "Save layout as", .hint = "Layout name",
                                                          .confirm = "Save layout"}, name);
        if (given != ui::DialogAnswer::None) answer = given;
        open = ImGui::IsPopupOpen("##prompt");
    }
};

} // namespace

TEST_CASE("A name prompt takes typing at once and Enter confirms it", "[ui][kit]") {
    ImGuiHarness h;
    Prompt prompt;
    h.set_ui([&] { prompt.draw(); });
    h.step(3);
    h.type("Wide");
    h.press(ImGuiKey_Enter);
    CHECK(prompt.name == "Wide");
    CHECK(prompt.answer == ui::DialogAnswer::Confirm);
    h.step();
    CHECK_FALSE(prompt.open);
}

TEST_CASE("A name prompt won't confirm without a name, and Esc cancels it", "[ui][kit]") {
    ImGuiHarness h;
    Prompt prompt;
    h.set_ui([&] { prompt.draw(); });
    h.step(3);
    h.press(ImGuiKey_Enter);
    CHECK(prompt.answer == ui::DialogAnswer::None);
    CHECK(prompt.open);

    h.press(ImGuiKey_Escape);
    CHECK(prompt.answer == ui::DialogAnswer::Cancel);
}

TEST_CASE("Closing with unsaved changes: Enter saves, Esc cancels", "[ui][kit]") {
    ImGuiHarness h;
    bool open_now = true;
    ui::UnsavedAnswer answer = ui::UnsavedAnswer::None;
    h.set_ui([&] {
        if (open_now) {
            ui::open_dialog("##unsaved");
            open_now = false;
        }
        const auto given = ui::unsaved_dialog("##unsaved", "Save changes to crate_wood.fjmat?",
                                              "Closing the tab without saving loses the changes.");
        if (given != ui::UnsavedAnswer::None) answer = given;
    });
    h.step(3);
    h.press(ImGuiKey_Enter);
    CHECK(answer == ui::UnsavedAnswer::Save);

    open_now = true;
    answer = ui::UnsavedAnswer::None;
    h.step(3);
    h.press(ImGuiKey_Escape);
    CHECK(answer == ui::UnsavedAnswer::Cancel);
}

TEST_CASE("Quitting lists everything unsaved and saves only what stays ticked", "[ui][kit]") {
    ImGuiHarness h;
    std::vector<ui::UnsavedItem> items{{.name = "crate_wood.fjmat"}, {.name = "hero.fjanimset"}};
    bool open_now = true;
    ui::UnsavedAnswer answer = ui::UnsavedAnswer::None;
    h.set_ui([&] {
        if (open_now) {
            ui::open_dialog("##quit");
            open_now = false;
        }
        const auto given = ui::unsaved_list_dialog("##quit", "2 assets have unsaved changes", items);
        if (given != ui::UnsavedAnswer::None) answer = given;
    });
    h.step(3);
    items[1].save = false;
    h.step();
    h.press(ImGuiKey_Enter);
    CHECK(answer == ui::UnsavedAnswer::Save);
    CHECK(items[0].save);
    CHECK_FALSE(items[1].save);
}

TEST_CASE("A dialog hangs from the point it is opened at", "[ui][kit]") {
    ImGuiHarness h;
    bool open_now = true;
    ImVec2 top_left{-1.0f, -1.0f};
    float width = 0.0f;
    h.set_ui([&] {
        if (open_now) {
            // Its left end at 100, flush under an edge at 50: a menu's dialog.
            ui::open_dialog("##hang", {{100.0f, 50.0f}, 0.0f});
            open_now = false;
        }
        (void)ui::confirm_dialog("##hang", {.title = "New script"}, [&] {
            top_left = ImGui::GetWindowPos();
            width = ImGui::GetWindowWidth();
        });
    });
    h.step(3);
    CHECK(top_left.x == 100.0f);
    CHECK(top_left.y == 50.0f);

    // Centred under a point: a tab's dialog under the header.
    open_now = true;
    h.set_ui([&] {
        if (open_now) {
            ImGui::CloseCurrentPopup();
            ui::open_dialog("##centred", {{400.0f, 80.0f}, 0.5f});
            open_now = false;
        }
        (void)ui::confirm_dialog("##centred", {.title = "Save changes?"}, [&] {
            top_left = ImGui::GetWindowPos();
            width = ImGui::GetWindowWidth();
        });
    });
    h.step(3);
    CHECK(top_left.x + width * 0.5f == 400.0f);
    CHECK(top_left.y == 80.0f);
}
