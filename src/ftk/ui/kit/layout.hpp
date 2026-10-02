#pragma once

// Where the next item goes, and how much room there is. Items go down the
// window, one under the other, from the cursor, unless the next is put on
// the same line as the last. Points are in screen pixels.
//
//     ui::button("Apply");
//     ui::same_line();
//     ui::button("Revert");
//     ui::separator();
//
//     ui::next_width(ui::space_left().x - ui::frame_height());
//     ui::next_width(ui::FILL);

#include <glm/vec2.hpp>

#include <limits>
#include <string_view>

namespace ftk::ui {

/// The next item goes on the same line as the last, the usual space after
/// it.
void same_line();

/// The same, `gap` pixels after it.
void same_line(float gap);

/// A rule across the space left, between groups of items.
void separator();

/// A little room under the last item, the usual space between groups.
void spacing();

/// Ends the line, or leaves an empty one when nothing is on it yet.
void new_line();

/// Where the next item goes.
[[nodiscard]] glm::vec2 cursor();

/// Puts the next item at `at`.
void set_cursor(glm::vec2 at);

/// The room from the cursor to the window's content edges.
[[nodiscard]] glm::vec2 space_left();

/// The next field, slider or list is `width` wide. Less than zero leaves
/// that much room at the right; FILL reaches the right edge.
void next_width(float width);

/// For next_width(): the next item reaches the right edge of the room.
inline constexpr float FILL = -std::numeric_limits<float>::min();

/// Takes `size` at the cursor as an item: room for what was drawn there by
/// hand.
void reserve(glm::vec2 size);

/// Text drawn next on this line sits level with a field's text beside it.
void align_text_to_frame();

/// The height of a field or a button: the text and its padding.
[[nodiscard]] float frame_height();

/// The height of a line of text.
[[nodiscard]] float line_height();

/// The room `text` takes on one line.
[[nodiscard]] glm::vec2 text_size(std::string_view text);

/// While it lives, what is drawn is one item to same_line() and the item_
/// queries: a field with its unit, a pill of buttons.
class Group {
public:
    Group();
    ~Group();
    Group(const Group&) = delete;
    Group& operator=(const Group&) = delete;
    Group(Group&&) = delete;
    Group& operator=(Group&&) = delete;
};

} // namespace ftk::ui
