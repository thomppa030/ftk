#pragma once

#include "ui/icons_lc.hpp"

// Icons by what they mean. Code says icon::remove, never a glyph macro, so an
// action has one icon everywhere and the icon library can be swapped here.
namespace fjell::ui::icon {

// Actions
inline constexpr const char* add          = ICON_LC_PLUS;
inline constexpr const char* remove       = ICON_LC_TRASH_2;
inline constexpr const char* clear        = ICON_LC_X;
inline constexpr const char* browse       = ICON_LC_FOLDER_OPEN;
inline constexpr const char* use_selected = ICON_LC_CROSSHAIR;
inline constexpr const char* save         = ICON_LC_SAVE;
inline constexpr const char* revert       = ICON_LC_UNDO_2;
inline constexpr const char* rename       = ICON_LC_PENCIL;
inline constexpr const char* duplicate    = ICON_LC_COPY;
inline constexpr const char* more         = ICON_LC_MORE_HORIZONTAL;
inline constexpr const char* search       = ICON_LC_SEARCH;
inline constexpr const char* reorder      = ICON_LC_GRIP_VERTICAL;

// What a section or panel is about
inline constexpr const char* properties        = ICON_LC_SLIDERS_HORIZONTAL;
inline constexpr const char* transform         = ICON_LC_MOVE_3D;
inline constexpr const char* components        = ICON_LC_PACKAGE;
inline constexpr const char* scripts           = ICON_LC_FILE_CODE;
inline constexpr const char* camera            = ICON_LC_CAMERA;
inline constexpr const char* directional_light = ICON_LC_SUN;
inline constexpr const char* point_light       = ICON_LC_LIGHTBULB;
inline constexpr const char* spot_light        = ICON_LC_FLASHLIGHT;
inline constexpr const char* volumetric_fog    = ICON_LC_CLOUD;
inline constexpr const char* fog_volume        = ICON_LC_CLOUD_FOG;
inline constexpr const char* world_environment = ICON_LC_GLOBE;
inline constexpr const char* source            = ICON_LC_LINK;
inline constexpr const char* snap              = ICON_LC_MAGNET;
inline constexpr const char* nothing_selected  = ICON_LC_MOUSE_POINTER_CLICK;

// Folding and dropdowns
inline constexpr const char* fold_open   = ICON_LC_CHEVRON_DOWN;
inline constexpr const char* fold_closed = ICON_LC_CHEVRON_RIGHT;
inline constexpr const char* dropdown    = ICON_LC_CHEVRON_DOWN;

// Feedback
inline constexpr const char* help    = ICON_LC_CIRCLE_HELP;
inline constexpr const char* warning = ICON_LC_TRIANGLE_ALERT;
inline constexpr const char* error   = ICON_LC_CIRCLE_X;
inline constexpr const char* success = ICON_LC_CIRCLE_CHECK;

} // namespace fjell::ui::icon
