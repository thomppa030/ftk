#pragma once

// Tooltips (sheet 5): shown once the mouse rests, after the short delay,
// wrapped at theme::TOOLTIP_WRAP, a shortcut on the right in the secondary
// colour.
//
//     if (ui::button("Bake")) bake();
//     ui::item_tooltip("Bakes every probe in the scene", "Ctrl B");
//
//     if (dragging) ui::tooltip("Drop to parent it here");

#include <string_view>

namespace ftk::ui {

/// The tooltip of the item just drawn, while the mouse rests on it.
void item_tooltip(std::string_view text, std::string_view shortcut = {});

/// A tooltip at the mouse this frame, whatever is under it: what a drag
/// would do, the value under a canvas's cursor. The last one asked for in a
/// frame is the one shown.
void tooltip(std::string_view text, std::string_view shortcut = {});

} // namespace ftk::ui
