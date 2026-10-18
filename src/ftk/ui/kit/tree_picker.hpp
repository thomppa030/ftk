#pragma once

// The drop-down a field opens to choose among a tree of choices: a search
// over the tree, its rows indented by depth, the chosen ones ticked in the
// toggle blue and the rows above them greyed (choosing one there adds
// nothing it doesn't say already), each row's own value at its end. It
// stays open, so several can be chosen; clicking a row, or Enter on the
// one the arrow keys reach, chooses or unchooses it. A last row may offer
// to add what was typed; the search keeps it, so the choice the caller makes
// of it shows next.
//
// The picker draws; it does not gather. Callers hand it the tree in order,
// each choice after the one above it, and act on what comes back, so what a
// choice is (a project's tag, a folder) stays the caller's business.
//
//     if (ui::icon_button("##add", icon::add, "Add a tag")) ui::tree_picker::open("##tags");
//     const auto done = ui::tree_picker::draw("##tags", choices, {.chosen = held});
//     if (done.toggled) toggle(*done.toggled);
//     if (done.added) add(*done.added);

#include <glm/vec2.hpp>

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ftk::ui::tree_picker {

/// One row of the tree.
struct Choice {
    /// Returned when it is chosen; unique within the picker.
    std::string value;
    /// The row's text: the last part of a dotted name, say.
    std::string label;
    /// How far it is indented: one more than the choice it lies under.
    int depth{0};
    /// A quiet word at the row's end: the whole name, say.
    std::string detail{};
};

struct Config {
    /// The values chosen now: ticked, and the rows above each greyed.
    std::span<const std::string> chosen{};
    const char* search_hint{"Search"};
    /// What the text typed would add as a new choice ("tree.oak.grey"); empty
    /// for nothing. A last row offers it while it isn't a choice already.
    std::function<std::string(std::string_view typed)> addable{};
    const char* empty_message{"Nothing to pick."};
    glm::vec2 size{320.0F, 400.0F};
};

/// What the picker did this frame.
struct Picked {
    /// A row chosen or unchosen: its value.
    std::optional<std::string> toggled{};
    /// The add row taken: what Config::addable gave for the text typed.
    std::optional<std::string> added{};
};

/// Opens the picker owned by `widget_id` on its next draw, under the
/// pointer, as grouped_picker::open() does.
void open(const char* widget_id);
/// The same, its top-left at `at` on screen.
void open(const char* widget_id, glm::vec2 at);
/// Whether `widget_id`'s picker is open.
[[nodiscard]] bool is_open(const char* widget_id);
/// Closes whichever picker is open.
void close();
/// Draws `widget_id`'s picker if it is the open one. The search takes the
/// keyboard as it opens; it shows the choices whose label or detail holds
/// what is typed, with the rows above them dimmed for where they are; Up and
/// Down move through the rows and Enter takes the one reached; Esc or a
/// click away closes it.
Picked draw(const char* widget_id, std::span<const Choice> choices, const Config& config);

} // namespace ftk::ui::tree_picker
