#include "kit_harness.hpp"

#include "ftk/ui/kit/dialog.hpp"
#include "ftk/ui/kit/input.hpp"
#include "ftk/ui/kit/popup.hpp"
#include "ftk/ui/kit/text_field.hpp"
#include "ftk/ui/kit/window.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using ftk::test::KitHarness;
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
        open = ui::popup_open("##prompt");
    }
};

} // namespace

TEST_CASE("A window's button bar confirms on Enter and cancels on Esc, unless something is typed", "[ui][kit]") {
    KitHarness h;
    ui::DialogAnswer answer = ui::DialogAnswer::None;
    bool can_confirm = true;
    std::string typed;
    h.set_ui([&] {
        (void)ui::text_entry("##field", typed);
        h.mark("field");
        const auto a = ui::window_bar({.title = "Import model", .confirm = "Import"}, {.can_confirm = can_confirm});
        if (a != ui::DialogAnswer::None) answer = a;
    });
    h.step(2);

    h.press(ui::Key::Enter);
    CHECK(answer == ui::DialogAnswer::Confirm);

    answer = ui::DialogAnswer::None;
    h.press(ui::Key::Escape);
    CHECK(answer == ui::DialogAnswer::Cancel);

    // Greyed out, Enter does nothing.
    answer = ui::DialogAnswer::None;
    can_confirm = false;
    h.press(ui::Key::Enter);
    CHECK(answer == ui::DialogAnswer::None);

    // Typing in a field, Enter and Esc are the field's.
    can_confirm = true;
    h.click("field");
    h.press(ui::Key::Enter);
    CHECK(answer == ui::DialogAnswer::None);
}

TEST_CASE("A name prompt takes typing at once and Enter confirms it", "[ui][kit]") {
    KitHarness h;
    Prompt prompt;
    h.set_ui([&] { prompt.draw(); });
    h.step(3);
    h.type("Wide");
    h.press(ui::Key::Enter);
    CHECK(prompt.name == "Wide");
    CHECK(prompt.answer == ui::DialogAnswer::Confirm);
    h.step();
    CHECK_FALSE(prompt.open);
}

TEST_CASE("A name prompt won't confirm without a name, and Esc cancels it", "[ui][kit]") {
    KitHarness h;
    Prompt prompt;
    h.set_ui([&] { prompt.draw(); });
    h.step(3);
    h.press(ui::Key::Enter);
    CHECK(prompt.answer == ui::DialogAnswer::None);
    CHECK(prompt.open);

    h.press(ui::Key::Escape);
    CHECK(prompt.answer == ui::DialogAnswer::Cancel);
}

TEST_CASE("Closing with unsaved changes: Enter saves, Esc cancels", "[ui][kit]") {
    KitHarness h;
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
    h.press(ui::Key::Enter);
    CHECK(answer == ui::UnsavedAnswer::Save);

    open_now = true;
    answer = ui::UnsavedAnswer::None;
    h.step(3);
    h.press(ui::Key::Escape);
    CHECK(answer == ui::UnsavedAnswer::Cancel);
}

TEST_CASE("Quitting lists everything unsaved and saves only what stays ticked", "[ui][kit]") {
    KitHarness h;
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
    h.press(ui::Key::Enter);
    CHECK(answer == ui::UnsavedAnswer::Save);
    CHECK(items[0].save);
    CHECK_FALSE(items[1].save);
}

TEST_CASE("Recovering counts what stays ticked; Discard and Esc answer otherwise", "[ui][kit]") {
    KitHarness h;
    std::vector<ui::UnsavedItem> items{{.name = "coin.cpp"}, {.name = "toon.fjsl"}};
    bool open_now = true;
    ui::RecoverAnswer answer = ui::RecoverAnswer::None;
    h.set_ui([&] {
        if (open_now) {
            ui::open_dialog("##recover");
            open_now = false;
        }
        const auto given = ui::recover_dialog("##recover", "Recover unsaved changes", "Two files were kept", items);
        if (given != ui::RecoverAnswer::None) answer = given;
    });
    h.step(3);
    REQUIRE(h.find("Recover 2 files", ftk::ui::core::Role::Button) != nullptr);
    items[1].save = false;
    h.step();
    const auto* recover = h.find("Recover 1 file", ftk::ui::core::Role::Button);
    REQUIRE(recover != nullptr);
    h.click_at(recover->rect.centre());
    CHECK(answer == ui::RecoverAnswer::Recover);

    open_now = true;
    answer = ui::RecoverAnswer::None;
    h.step(3);
    h.click_at(h.find("Discard", ftk::ui::core::Role::Button)->rect.centre());
    CHECK(answer == ui::RecoverAnswer::Discard);

    open_now = true;
    answer = ui::RecoverAnswer::None;
    h.step(3);
    h.press(ui::Key::Escape);
    CHECK(answer == ui::RecoverAnswer::Later);
}

TEST_CASE("A dialog hangs from the point it is opened at", "[ui][kit]") {
    KitHarness h;
    bool open_now = true;
    glm::vec2 top_left{-1.0f, -1.0f};
    float width = 0.0f;
    h.set_ui([&] {
        if (open_now) {
            // Its left end at 100, flush under an edge at 50: a menu's dialog.
            ui::open_dialog("##hang", {{100.0f, 50.0f}, 0.0f});
            open_now = false;
        }
        (void)ui::confirm_dialog("##hang", {.title = "New script"}, [&] {
            top_left = ui::window_pos();
            width = ui::window_size().x;
        });
    });
    h.step(3);
    CHECK(top_left.x == 100.0f);
    CHECK(top_left.y == 50.0f);

    // Centred under a point: a tab's dialog under the header.
    open_now = true;
    h.set_ui([&] {
        if (open_now) {
            ui::close_popup();
            ui::open_dialog("##centred", {{400.0f, 80.0f}, 0.5f});
            open_now = false;
        }
        (void)ui::confirm_dialog("##centred", {.title = "Save changes?"}, [&] {
            top_left = ui::window_pos();
            width = ui::window_size().x;
        });
    });
    h.step(3);
    CHECK(top_left.x + width * 0.5f == 400.0f);
    CHECK(top_left.y == 80.0f);
}

TEST_CASE("A prompt opened again after a cancel takes typing afresh", "[ui][kit]") {
    KitHarness h;
    Prompt prompt;
    h.set_ui([&] { prompt.draw(); });
    h.step(2);
    h.type("x");
    REQUIRE(prompt.name == "x");
    h.press(ui::Key::Escape);
    h.step(2);
    REQUIRE(prompt.answer == ui::DialogAnswer::Cancel);
    // Its name emptied before it opens again, as a prompt's is.
    prompt = Prompt{};
    h.step(2);
    h.type("y");
    REQUIRE(prompt.name == "y");
}

TEST_CASE("A prompt answered gives the keyboard back to what had it as it opened", "[ui][kit]") {
    KitHarness h;
    // A widget that takes the keyboard when pressed and keeps what is typed
    // while it has it, as a code view does.
    std::string typed;
    bool asking = false;
    Prompt prompt;
    h.set_ui([&] {
        ui::core::Context& context = ui::core::current();
        const ui::Id id = context.id("##typing");
        const ui::core::Rect rect{.min = {10.0f, 10.0f}, .max = {200.0f, 40.0f}};
        (void)context.interact(id, rect, {});
        if (context.focusable(id, {.text = true}).focused) typed += context.typed();
        if (asking) prompt.draw();
    });
    h.step(2);
    h.click_at({100.0f, 25.0f});
    h.type("a");
    REQUIRE(typed == "a");

    // The prompt takes the keyboard; Escape answers it, and typing goes on
    // where it was.
    asking = true;
    h.step(2);
    h.type("x");
    REQUIRE(prompt.name == "x");
    h.press(ui::Key::Escape);
    h.step(2);
    REQUIRE(prompt.answer == ui::DialogAnswer::Cancel);
    h.type("b");
    REQUIRE(typed == "ab");

    // The same once Enter confirms.
    prompt = Prompt{};
    h.step(2);
    h.type("y");
    h.press(ui::Key::Enter);
    h.step(2);
    REQUIRE(prompt.answer == ui::DialogAnswer::Confirm);
    h.type("c");
    REQUIRE(typed == "abc");
}
