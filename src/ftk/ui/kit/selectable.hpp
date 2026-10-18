#pragma once

// A line that can be picked: a log entry, a file, a step of the history.
// It lights under the mouse and while picked; a list of them is the
// caller's to keep.
//
//     for (std::size_t i = 0; i < entries.size(); ++i) {
//         const ui::IdScope id(static_cast<int>(i));
//         if (ui::selectable("##entry", i == picked, {.text = entries[i]})) picked = i;
//     }

#include <glm/vec2.hpp>

#include <string_view>

namespace ftk::ui {

struct SelectableOptions {
    /// Across every column of the table row it is in.
    bool span_columns{false};
    /// A double click picks it too; mouse_double_clicked() tells which.
    bool double_click{false};
    /// An item drawn over it afterwards takes the mouse there.
    bool overlap{false};
    /// Its size: zero across fills the room, zero down is a line of text.
    glm::vec2 size{0.0f, 0.0f};
    /// What it shows, as it is, when that is not the caller's own label: a
    /// log message or a file name, which may hold ##. The label is then
    /// only its id. Empty shows the label up to its ##.
    std::string_view text{};
    /// What it is called to what reads the screen, and to tests, when it
    /// draws nothing of its own to go by: a menu's entry, which draws its
    /// label itself. Empty: what it shows.
    std::string_view name{};
    /// Picked in a popup, it leaves the popup open: a list several are
    /// chosen from, each click choosing or unchoosing one.
    bool keeps_popup_open{false};
};

/// `label` (an id after ##) as a line that can be picked, lit while
/// `selected`. Returns true when clicked.
bool selectable(const char* label, bool selected, const SelectableOptions& options = {});

} // namespace ftk::ui
