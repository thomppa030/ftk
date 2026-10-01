#pragma once

// The foot of a window that edits something saved to a file (project
// settings): whether there are unsaved changes, then Revert and Save on
// the right.

namespace ftk::ui {

enum class SaveAction { None, Save, Revert };

/// The bar's height, to leave room for it under the window's content.
[[nodiscard]] float save_bar_height();

/// The bar, across the window's full width at the cursor: the unsaved dot
/// and "Unsaved changes" while `unsaved`, then Revert (ghost) and Save
/// (primary), both greyed out while there is nothing to save. `saves`
/// names what Save writes, for its tooltip ("settings.json"). Returns what
/// was clicked.
SaveAction save_bar(bool unsaved, const char* saves);

} // namespace ftk::ui
