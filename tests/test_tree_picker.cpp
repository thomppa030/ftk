#include "kit_harness.hpp"

#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/tree_picker.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <string>
#include <utility>
#include <vector>

using ftk::test::KitHarness;
namespace ui = ftk::ui;
namespace picker = ftk::ui::tree_picker;

namespace {

// A picker over a small tree of tags, open under a button, choosing and
// unchoosing as a Tags row does, and adding what is typed in lower case.
struct OpenPicker {
    KitHarness h;
    std::vector<picker::Choice> choices{
        {.value = "rock", .label = "rock", .depth = 0, .detail = "rock"},
        {.value = "tree", .label = "tree", .depth = 0, .detail = "tree"},
        {.value = "tree.birch", .label = "birch", .depth = 1, .detail = "tree.birch"},
        {.value = "tree.oak", .label = "oak", .depth = 1, .detail = "tree.oak"},
        {.value = "tree.oak.grey", .label = "grey", .depth = 2, .detail = "tree.oak.grey"},
    };
    std::vector<std::string> chosen;
    std::vector<std::string> added;
    bool open_next = true;
    bool is_open = false;

    OpenPicker() {
        h.set_ui([this] {
            ui::button("Tags");
            if (std::exchange(open_next, false)) picker::open("##tags");
            const picker::Picked done =
                picker::draw("##tags", choices,
                             {.chosen = chosen, .addable = [](std::string_view typed) {
                                  std::string name(typed);
                                  for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                                  return name;
                              }});
            if (done.toggled) {
                if (std::erase(chosen, *done.toggled) == 0) chosen.push_back(*done.toggled);
            }
            // As a Tags row does: the new name joins the tree, chosen.
            if (done.added) {
                added.push_back(*done.added);
                choices.push_back({.value = *done.added, .label = *done.added, .depth = 0, .detail = *done.added});
                chosen.push_back(*done.added);
            }
            is_open = picker::is_open("##tags");
        });
        h.step(3);
    }
    ~OpenPicker() { picker::close(); }

    // The rows the last frame showed, by value.
    std::vector<std::string> rows() {
        std::vector<std::string> shown;
        for (const ui::core::SemanticNode& node : h.ui().semantics()) {
            if (node.role == ui::core::Role::ListItem) shown.emplace_back(node.label);
        }
        return shown;
    }
};

} // namespace

TEST_CASE("A tree picker shows its tree, chooses by click or Enter, and stays open", "[ui][picker]") {
    OpenPicker p;
    REQUIRE(p.is_open);
    CHECK(p.rows() == std::vector<std::string>{"rock", "tree", "tree.birch", "tree.oak", "tree.oak.grey"});
    p.h.press(ui::Key::Enter);
    p.h.step();
    CHECK(p.chosen == std::vector<std::string>{"rock"});
    p.h.click("tree.oak");
    p.h.step();
    CHECK(p.chosen == std::vector<std::string>{"rock", "tree.oak"});
    CHECK(p.is_open);
    // Chosen again, it comes off.
    p.h.click("rock");
    p.h.step();
    CHECK(p.chosen == std::vector<std::string>{"tree.oak"});
}

TEST_CASE("Searching a tree picker shows the matches with the rows above them", "[ui][picker]") {
    OpenPicker p;
    p.h.type("gre");
    p.h.step(2);
    // What was typed is offered too, last, as a name to add.
    CHECK(p.rows() == std::vector<std::string>{"tree", "tree.oak", "tree.oak.grey", "##add"});
    // Enter takes the first match, not a row kept for where it lies.
    p.h.press(ui::Key::Enter);
    p.h.step();
    CHECK(p.chosen == std::vector<std::string>{"tree.oak.grey"});
}

TEST_CASE("A tree picker offers to add what was typed when it is no choice", "[ui][picker]") {
    OpenPicker p;
    p.h.type("Tree.Oak.Brown");
    p.h.step(2);
    CHECK(p.rows() == std::vector<std::string>{"##add"});
    p.h.press(ui::Key::Enter);
    p.h.step(2);
    CHECK(p.added == std::vector<std::string>{"tree.oak.brown"});
    // The search keeps it: the new choice shows, chosen, and isn't offered again.
    CHECK(p.rows() == std::vector<std::string>{"tree.oak.brown"});
    CHECK(p.is_open);
}

TEST_CASE("A choice typed out whole is offered as itself, never to add", "[ui][picker]") {
    OpenPicker p;
    p.h.type("tree.oak");
    p.h.step(2);
    CHECK(p.rows() == std::vector<std::string>{"tree", "tree.oak", "tree.oak.grey"});
}

TEST_CASE("Esc closes a tree picker", "[ui][picker]") {
    OpenPicker p;
    p.h.press(ui::Key::Escape);
    p.h.step(2);
    CHECK_FALSE(p.is_open);
}
