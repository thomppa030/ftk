#pragma once

// Who a program is to the desktop: told first thing in main, before any
// window opens.
//
//     (void)ftk::set_app_identity({.id = "fjedit", .name = "fjedit", .icon = brand / "fjedit-tile.svg"});

#include "ftk/base/result.hpp"
#include "ftk/image/rgba8.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

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

/// The sizes a program's icon is drawn at, largest first: what title bars,
/// taskbars and window switchers ask for.
inline constexpr std::array<uint32_t, 7> APP_ICON_SIZES{256, 128, 64, 48, 32, 24, 16};

/// The SVG file `svg` drawn at each of APP_ICON_SIZES, that many pixels tall,
/// as wide as its proportions make it; why not when it doesn't draw.
[[nodiscard]] Result<std::vector<image::Rgba8>> draw_app_icon(std::string_view svg);

/// Tells the platform who the program is: the id and name, and the icon every
/// window opened from now on carries. Before the first window opens, or a
/// window manager may have taken another id. Why not when the icon doesn't
/// read or draw; the id and name are told even then.
Result<> set_app_identity(const AppIdentity& identity);

} // namespace ftk
