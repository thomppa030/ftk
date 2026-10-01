#pragma once

#include "ftk/ui/icons_lc.hpp"

// The icons the kit's own widgets draw, by what they mean, so an action has
// one icon everywhere and the icon library can be swapped here. A program
// names its own icons, and chooses their glyphs, in a file of its own
// (ftk_check_editor_ui's ICONS).
namespace ftk::ui::icon {

// Actions
inline constexpr const char* add     = ICON_LC_PLUS;
inline constexpr const char* minus   = ICON_LC_MINUS;
inline constexpr const char* remove  = ICON_LC_TRASH_2;
inline constexpr const char* clear   = ICON_LC_X;
inline constexpr const char* save    = ICON_LC_SAVE;
inline constexpr const char* revert  = ICON_LC_UNDO_2;
inline constexpr const char* search  = ICON_LC_SEARCH;
inline constexpr const char* reorder = ICON_LC_GRIP_VERTICAL;
inline constexpr const char* follow  = ICON_LC_ARROW_DOWN_WIDE_NARROW;  // keep the newest line in view

// The app's own panels
inline constexpr const char* console = ICON_LC_TERMINAL;
inline constexpr const char* history = ICON_LC_HISTORY;

// The file browser's places
inline constexpr const char* folder    = ICON_LC_FOLDER;
inline constexpr const char* file      = ICON_LC_FILE;
inline constexpr const char* back      = ICON_LC_ARROW_LEFT;    // where the browser was before
inline constexpr const char* up_folder = ICON_LC_ARROW_UP;      // the folder this one is in
inline constexpr const char* home      = ICON_LC_HOUSE;         // the user's home folder
inline constexpr const char* downloads = ICON_LC_DOWNLOAD;
inline constexpr const char* project   = ICON_LC_FOLDER_GIT_2;  // the open project's folder

// Kinds of asset every program has (ftk/ui/kit/asset_kind.hpp)
inline constexpr const char* texture = ICON_LC_IMAGE;
inline constexpr const char* audio   = ICON_LC_AUDIO_LINES;
inline constexpr const char* font    = ICON_LC_TYPE;

// Menus, folding and dropdowns
inline constexpr const char* checked     = ICON_LC_CHECK;
inline constexpr const char* fold_open   = ICON_LC_CHEVRON_DOWN;
inline constexpr const char* fold_closed = ICON_LC_CHEVRON_RIGHT;
inline constexpr const char* dropdown    = ICON_LC_CHEVRON_DOWN;

// Feedback
inline constexpr const char* help    = ICON_LC_CIRCLE_HELP;
inline constexpr const char* warning = ICON_LC_TRIANGLE_ALERT;
inline constexpr const char* error   = ICON_LC_CIRCLE_X;
inline constexpr const char* success = ICON_LC_CIRCLE_CHECK;
inline constexpr const char* refused = ICON_LC_BAN;

} // namespace ftk::ui::icon
