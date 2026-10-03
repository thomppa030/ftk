#pragma once

// The example app's own icons, by what they mean, as a program names its
// icons in a file of its own (ftk_check_editor_ui's ICONS); the kit's are
// ftk/ui/kit/icons.hpp.

#include "ftk/ui/icons_lc.hpp"

namespace ftk::example::icon {

// The Shell page's panels, as the editor's.
inline constexpr const char* hierarchy = ICON_LC_LIST_TREE;
inline constexpr const char* viewport = ICON_LC_MONITOR;
inline constexpr const char* inspector = ICON_LC_SLIDERS_HORIZONTAL;
inline constexpr const char* scene = ICON_LC_BOX;

} // namespace ftk::example::icon
