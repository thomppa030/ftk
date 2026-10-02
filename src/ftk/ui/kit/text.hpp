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

#include <string_view>

namespace ftk::ui {

void text(std::string_view text);

/// In `colour`, a theme token.
void text(std::string_view text, Color colour);

/// The size of the text drawn now, in pixels.
[[nodiscard]] float font_size();

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
