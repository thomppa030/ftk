#pragma once

// Who a program is to the desktop: told first thing in main, before any
// window opens.
//
//     (void)ftk::set_app_identity({.id = "fjedit", .name = "fjedit", .icon = brand / "fjedit-tile.svg"});

#include "ftk/base/result.hpp"

#include <filesystem>
#include <string>

namespace ftk {

/// What a program is to the desktop.
struct AppIdentity {
    /// What the window manager and a desktop entry know it by: its program's
    /// name, "fjell-hub".
    std::string id;
    /// Its name as people read it, "fjell".
    std::string name;
    /// Its icon, an SVG file: a square, the desktop tile.
    std::filesystem::path icon;
};

/// Tells the platform who the program is: the id and name, and the icon every
/// window opened from now on carries, drawn at image::ICON_SIZES. Before the
/// first window opens, or a window manager may have taken another id. Why not
/// when the icon doesn't read or draw; the id and name are told even then.
Result<> set_app_identity(const AppIdentity& identity);

} // namespace ftk
