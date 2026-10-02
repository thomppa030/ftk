#pragma once

// Text as an item: a line in the text colour, or in a token of its own.
// A label, a hint, a field and the like have their own pieces of the kit;
// this is for the text between them.
//
//     ui::text("Nothing to bake");
//     ui::text(path, theme::text_secondary());

#include "ftk/ui/types.hpp"

#include <string_view>

namespace ftk::ui {

void text(std::string_view text);

/// In `colour`, a theme token.
void text(std::string_view text, Color colour);

} // namespace ftk::ui
