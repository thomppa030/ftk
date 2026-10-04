#pragma once

// The foot of a window that edits something saved to a file (project
// settings): whether there are unsaved changes, then Revert and Save on
// the right. A window whose changes apply at once (preferences) has a note
// bar there instead, saying where they are kept.

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

/// The note bar, across the window's full width at the cursor and as tall
/// as the save bar: `icon` in the muted colour, then `text` ("Kept on this
/// machine"). Nothing in it is pressed.
void note_bar(const char* icon, const char* text);

} // namespace ftk::ui
