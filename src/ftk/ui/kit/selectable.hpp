#pragma once

// A line that can be picked: a log entry, a file, a step of the history.
// It lights under the mouse and while picked; a list of them is the
// caller's to keep.
//
//     for (std::size_t i = 0; i < entries.size(); ++i) {
//         if (ui::selectable(entries[i].c_str(), i == picked)) picked = i;
//     }

#include <glm/vec2.hpp>

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
};

/// `label` (an id after ##) as a line that can be picked, lit while
/// `selected`. Returns true when clicked.
bool selectable(const char* label, bool selected, const SelectableOptions& options = {});

} // namespace ftk::ui
