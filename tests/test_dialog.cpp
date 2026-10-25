#include "kit_harness.hpp"

#include "ftk/ui/kit/dialog.hpp"
#include "ftk/ui/kit/icons.hpp"
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

TEST_CASE("A window's button bar names its no as the dialog asks, and Esc still presses it", "[ui][kit]") {
    KitHarness h;
    ui::DialogAnswer answer = ui::DialogAnswer::None;
    const char* cancel = "Not now";
    h.set_ui([&] {
        const auto a = ui::window_bar({.title = "Import models", .confirm = "Import 179", .cancel = cancel}, {});
        if (a != ui::DialogAnswer::None) answer = a;
    });
    h.step(2);
    const auto* not_now = h.find("Not now", ftk::ui::core::Role::Button);
    REQUIRE(not_now != nullptr);
    CHECK(h.find("Cancel", ftk::ui::core::Role::Button) == nullptr);
    h.click_at(not_now->rect.centre());
    CHECK(answer == ui::DialogAnswer::Cancel);

    // A longer word widens its button, the verb still at the right edge, clear of it.
    cancel = "Close and leave the rest raw";
    answer = ui::DialogAnswer::None;
    h.step(2);
    const auto* close = h.find(cancel, ftk::ui::core::Role::Button);
    const auto* verb = h.find("Import 179", ftk::ui::core::Role::Button);
    REQUIRE(close != nullptr);
    REQUIRE(verb != nullptr);
    CHECK(close->rect.max.x <= verb->rect.min.x);
    CHECK(close->rect.width() > 96.0f);
    h.press(ui::Key::Escape);
    CHECK(answer == ui::DialogAnswer::Cancel);
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

TEST_CASE("An unsaved line shows up to three changes, more folded under a count", "[ui][kit]") {
    KitHarness h{{800.0f, 900.0f}};
    std::vector<ui::UnsavedItem> items{
        {.name = "sword.fjmesh", .changes = {"Tags: weapons.sword", "Tags: weapons.axe"}},
        {.name = "Project settings", .changes = {"a", "b", "c", "d", "e"}},
    };
    bool open_now = true;
    h.set_ui([&] {
        if (open_now) {
            ui::open_dialog("##quit");
            open_now = false;
        }
        (void)ui::unsaved_list_dialog("##quit", "Unsaved", items);
    });
    h.step(3);
    const std::string closed = std::string(ui::icon::fold_closed) + "  5 changes";
    REQUIRE(h.find(closed) != nullptr);
    CHECK(h.find(std::string(ui::icon::fold_closed) + "  2 changes") == nullptr);
    h.click(closed);
    h.step(2);
    CHECK(items[1].changes_open);
    CHECK(h.find(std::string(ui::icon::fold_open) + "  5 changes") != nullptr);
}

TEST_CASE("The unsaved list stops growing at its tallest and scrolls", "[ui][kit]") {
    KitHarness h{{800.0f, 1400.0f}};
    std::vector<std::string> many;
    for (int i = 0; i < 40; ++i) many.push_back("Change " + std::to_string(i));
    std::vector<ui::UnsavedItem> items{{.name = "Project settings", .changes = many}};
    bool open_now = true;
    h.set_ui([&] {
        if (open_now) {
            ui::open_dialog("##quit");
            open_now = false;
        }
        (void)ui::unsaved_list_dialog("##quit", "Unsaved", items, "close");
    });
    h.step(3);
    const float folded = h.rect("Cancel").min.y;
    items[0].changes_open = true;
    h.step(3);
    // Forty lines would push the buttons far further down.
    CHECK(h.rect("Cancel").min.y - folded <= 330.0f);
    CHECK(h.rect("Cancel").min.y - folded > 100.0f);
}

TEST_CASE("The unsaved list says what saving lets happen", "[ui][kit]") {
    KitHarness h;
    std::vector<ui::UnsavedItem> items{{.name = "forest.fjell"}, {.name = "Project settings"}};
    bool open_now = true;
    h.set_ui([&] {
        if (open_now) {
            ui::open_dialog("##close");
            open_now = false;
        }
        (void)ui::unsaved_list_dialog("##close", "Unsaved", items, "close");
    });
    h.step(3);
    CHECK(h.find(std::string(ui::icon::save) + "  Save 2 and close") != nullptr);
    items[0].save = false;
    items[1].save = false;
    h.step(2);
    CHECK(h.find(std::string(ui::icon::save) + "  Close") != nullptr);
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

namespace {

// The clash dialog over `items`, opened on the first frame; `answer` keeps
// the last answer it gave.
struct Clashes {
    std::vector<ui::ClashItem> items;
    bool open_now{true};
    ui::DialogAnswer answer{ui::DialogAnswer::None};

    void draw() {
        if (open_now) {
            ui::open_clash_dialog("##clash", {{400.0f, 0.0f}, 0.5f});
            open_now = false;
        }
        const auto given = ui::clash_dialog(
            "##clash", {.title = "Replace the 3 files already in props?", .copied = "9 other files were copied.",
                        .folder = "props"},
            items);
        if (given != ui::DialogAnswer::None) answer = given;
    }
};

std::vector<ui::ClashItem> three_clashes() {
    return {{.name = "crate.gltf", .here = "398 KB, 2 Oct 14:20", .dropped = "412 KB, today 09:12",
             .replace_note = "crate.fjmesh comes back changed"},
            {.name = "lantern.glb", .here = "1.2 MB, 30 Sep 11:05", .dropped = "1.2 MB, today 09:12",
             .kept_name = "lantern_1.glb"},
            {.name = "rug", .here = "a folder", .dropped = "2.1 MB, today 09:12", .kept_name = "rug_1",
             .replaceable = false}};
}

// The `n`th segment labelled `label` the last frame drew, counting the
// "Every file" row's as the first.
const ftk::ui::core::SemanticNode* segment(KitHarness& h, std::string_view label, int n) {
    for (const auto& node : h.ui().semantics()) {
        if (node.role == ftk::ui::core::Role::Button && node.label == label && n-- == 0) return &node;
    }
    return nullptr;
}

} // namespace

TEST_CASE("The clash dialog asks each name at Skip, counting what it copies", "[ui][kit]") {
    KitHarness h;
    Clashes clashes{.items = three_clashes()};
    h.set_ui([&] { clashes.draw(); });
    h.step(3);
    // Every row starts at Skip: nothing to copy, the verb greyed out.
    REQUIRE(h.find("Copy 0 files", ftk::ui::core::Role::Button) != nullptr);
    // The folder can't be written over: no Replace on its row.
    CHECK(segment(h, "Replace", 2) != nullptr);
    CHECK(segment(h, "Replace", 3) == nullptr);
    CHECK(segment(h, "Keep both", 3) != nullptr);

    h.click_at(segment(h, "Keep both", 2)->rect.centre());
    CHECK(clashes.items[1].choice == ui::ClashChoice::KeepBoth);
    const auto* copy = h.find("Copy 1 file", ftk::ui::core::Role::Button);
    REQUIRE(copy != nullptr);
    // Nothing replaced: Enter copies.
    h.press(ui::Key::Enter);
    CHECK(clashes.answer == ui::DialogAnswer::Confirm);
}

TEST_CASE("Replacing makes the clash dialog the danger kind, Enter cancelling", "[ui][kit]") {
    KitHarness h;
    Clashes clashes{.items = three_clashes()};
    h.set_ui([&] { clashes.draw(); });
    h.step(3);
    h.click_at(segment(h, "Replace", 1)->rect.centre());
    CHECK(clashes.items[0].choice == ui::ClashChoice::Replace);
    REQUIRE(h.find("Copy 1 file", ftk::ui::core::Role::Button) != nullptr);
    h.press(ui::Key::Enter);
    CHECK(clashes.answer == ui::DialogAnswer::Cancel);

    clashes.open_now = true;
    clashes.answer = ui::DialogAnswer::None;
    h.step(3);
    h.click_at(h.find("Copy 1 file", ftk::ui::core::Role::Button)->rect.centre());
    CHECK(clashes.answer == ui::DialogAnswer::Confirm);
}

TEST_CASE("Every file sets every row, Replace leaving a folder as it is", "[ui][kit]") {
    KitHarness h;
    Clashes clashes{.items = three_clashes()};
    h.set_ui([&] { clashes.draw(); });
    h.step(3);
    h.click_at(segment(h, "Keep both", 0)->rect.centre());
    for (const ui::ClashItem& item : clashes.items) CHECK(item.choice == ui::ClashChoice::KeepBoth);
    h.step();
    REQUIRE(h.find("Copy 3 files", ftk::ui::core::Role::Button) != nullptr);

    h.click_at(segment(h, "Replace", 0)->rect.centre());
    CHECK(clashes.items[0].choice == ui::ClashChoice::Replace);
    CHECK(clashes.items[1].choice == ui::ClashChoice::Replace);
    CHECK(clashes.items[2].choice == ui::ClashChoice::KeepBoth);

    h.press(ui::Key::Escape);
    CHECK(clashes.answer == ui::DialogAnswer::Cancel);
}

namespace {

// A list dialog that deletes, opened on the first frame; `answer` keeps the
// last answer it gave.
struct DeleteList {
    std::vector<ui::ListedItem> items;
    bool open_now{true};
    ui::DialogAnswer answer{ui::DialogAnswer::None};

    void draw() {
        if (open_now) {
            ui::open_dialog("##delete");
            open_now = false;
        }
        const auto given = ui::list_dialog("##delete", {.title = "Delete 2 files?", .confirm = "Delete 2 files",
                                                        .destructive = true},
                                           "They are deleted from disk. This can't be undone.", items,
                                           "1 other file uses them");
        if (given != ui::DialogAnswer::None) answer = given;
    }
};

} // namespace

TEST_CASE("A list dialog that destroys cancels on Enter and deletes only from its button", "[ui][kit]") {
    KitHarness h;
    DeleteList dialog{.items = {{.name = "rock_01.fjmesh", .detail = "and rock_01.glb, its source",
                                 .warning = "Used by scenes/shore.fjell"},
                                {.name = "moss.fjmat"}}};
    h.set_ui([&] { dialog.draw(); });
    h.step(3);
    REQUIRE(h.find("Delete 2 files") != nullptr);
    h.press(ui::Key::Enter);
    CHECK(dialog.answer == ui::DialogAnswer::Cancel);

    dialog.answer = ui::DialogAnswer::None;
    dialog.open_now = true;
    h.step(3);
    h.click("Delete 2 files");
    CHECK(dialog.answer == ui::DialogAnswer::Confirm);
}

TEST_CASE("A warning under a listed thing takes a line of its own", "[ui][kit]") {
    KitHarness h;
    DeleteList dialog{.items = {{.name = "rock_01.fjmesh"}, {.name = "moss.fjmat"}}};
    h.set_ui([&] { dialog.draw(); });
    h.step(3);
    const float plain = h.rect("Cancel").min.y;
    dialog.items[0].warning = "Used by scenes/shore.fjell";
    h.step(3);
    CHECK(h.rect("Cancel").min.y > plain + 8.0f);
}

TEST_CASE("A list dialog stops growing at its tallest and scrolls", "[ui][kit]") {
    KitHarness h{{800.0f, 1400.0f}};
    DeleteList dialog{.items = {{.name = "rock_01.fjmesh"}}};
    h.set_ui([&] { dialog.draw(); });
    h.step(3);
    const float one = h.rect("Cancel").min.y;
    for (int i = 0; i < 40; ++i) dialog.items.push_back({.name = "rock_" + std::to_string(i) + ".fjmesh"});
    h.step(3);
    // Forty rows would push the buttons far further down.
    CHECK(h.rect("Cancel").min.y - one <= 330.0f);
    CHECK(h.rect("Cancel").min.y - one > 100.0f);
}
