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

// The Performance page's panel and its sections.
inline constexpr const char* performance = ICON_LC_GAUGE;
inline constexpr const char* frame = ICON_LC_ACTIVITY;
inline constexpr const char* idle = ICON_LC_PAUSE;
inline constexpr const char* atlas = ICON_LC_TYPE;
inline constexpr const char* load = ICON_LC_LAYERS_3;

} // namespace ftk::example::icon
