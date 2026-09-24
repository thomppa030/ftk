#include "imgui_harness.hpp"
#include "ui/kit/dialog.hpp"

#include <catch2/catch_test_macros.hpp>
#include <imgui_internal.h>

#include <string>

using fjell::test::ImGuiHarness;
namespace ui = fjell::ui;

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
