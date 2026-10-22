#pragma once

// Text as an item: a line in the text colour, or in a token of its own.
// A label, a hint, a field and the like have their own pieces of the kit;
// this is for the text between them. And the face, size and colour of the
// text drawn while a scope lives.
//
//     ui::text("Nothing to bake");
//     ui::text(path, theme::text_secondary());
//
//     {
//         ui::FontScope mono(theme::mono_font(), theme::SMALL_TEXT);
//         ui::TextColour dim(theme::text_secondary());
//         ui::text(hash);
//     }

#include "ftk/ui/types.hpp"

#include <glm/vec2.hpp>

#include <string>
#include <string_view>

namespace ftk::ui {

void text(std::string_view text);

/// In `colour`, a theme token.
void text(std::string_view text, Color colour);

/// On one line at most `width` pixels wide, in `colour`: whole when it
/// fits, else cut with an ellipsis and whole in the tooltip, as a tree row
/// cuts its name. For text that leaves room beside it, as a path before
/// its button.
void cut_text(std::string_view text, float width, Color colour);

/// Led by a bullet: a line of a list in a dialog.
void bullet_text(std::string_view text);

/// A change as an undo step names it, the values it marks in colour, as
/// ftk::describe_change() writes them (\x01 before \x02, \x03 after \x04):
/// the value before in the error colour, after in the success colour, the
/// rest in `plain`.
void marked_text(std::string_view text, Color plain);
/// marked_text() drawn at `at` without taking room: over an item placed
/// already, as a list row's label.
void draw_marked_text(glm::vec2 at, std::string_view text, Color plain);
/// `text` without the marks marked_text() reads, for drawing it in one
/// colour or naming it.
[[nodiscard]] std::string unmarked(std::string_view text);

/// The size of the text drawn now, in pixels.
[[nodiscard]] float font_size();

/// The colour text is drawn in now: the text token, or a TextColour's.
[[nodiscard]] Color text_colour();

/// While it lives, text is drawn in `font` (the current face when none) at
/// `size` pixels, or at the current size when `size` is 0.
class FontScope {
public:
    FontScope(Font font, float size);
    ~FontScope();
    FontScope(const FontScope&) = delete;
    FontScope& operator=(const FontScope&) = delete;
    FontScope(FontScope&&) = delete;
    FontScope& operator=(FontScope&&) = delete;
};

/// While it lives, text is drawn in `colour`, a theme token.
class TextColour {
public:
    explicit TextColour(Color colour);
    ~TextColour();
    TextColour(const TextColour&) = delete;
    TextColour& operator=(const TextColour&) = delete;
    TextColour(TextColour&&) = delete;
    TextColour& operator=(TextColour&&) = delete;
};

} // namespace ftk::ui
