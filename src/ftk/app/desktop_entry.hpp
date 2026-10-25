#pragma once

// A program's place in the desktop's launcher on Linux (rofi, an application
// menu): a desktop entry naming the program, and its icon in the hicolor
// theme, both under the user's data folder. Other systems list programs by
// their installer's shortcuts, which aren't these.
//
//     const ftk::DesktopEntry entry{.id = "fjedit", .name = "fjedit", ...};
//     if (ftk::HAS_DESKTOP_ENTRIES) (void)ftk::create_desktop_entry(entry, ftk::desktop_data_home());

#include "ftk/base/result.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ftk {

/// Whether this system lists programs by desktop entries.
#ifdef __linux__
inline constexpr bool HAS_DESKTOP_ENTRIES = true;
#else
inline constexpr bool HAS_DESKTOP_ENTRIES = false;
#endif

/// What a desktop entry says of its program.
struct DesktopEntry {
    /// The entry's file name and its icon's, and what the window manager knows
    /// the program by (its app id): "fjell-hub".
    std::string id{};
    /// The program's name as the launcher lists it: "fjell".
    std::string name{};
    /// One line of what it is: "Game engine".
    std::string comment{};
    /// The launcher's sections it belongs to: "Development", "Graphics".
    std::vector<std::string> categories{};
    /// The program it opens.
    std::filesystem::path program{};
    /// Its icon, an SVG file: kept as it is, and drawn at the theme's sizes.
    std::filesystem::path icon{};
};

/// The sizes an icon is written into the hicolor theme at, besides the SVG.
inline constexpr std::array<uint32_t, 8> DESKTOP_ICON_SIZES{16, 24, 32, 48, 64, 128, 256, 512};

/// Where desktop entries and icons are kept for this user: $XDG_DATA_HOME,
/// else ~/.local/share; empty where there are no desktop entries, or no home.
[[nodiscard]] std::filesystem::path desktop_data_home();

/// The program the desktop entry `id` under `data_home` opens; nothing when
/// there is no such entry or it opens nothing.
[[nodiscard]] std::optional<std::filesystem::path> desktop_entry_program(std::string_view id,
                                                                         const std::filesystem::path& data_home);

/// Writes `entry` under `data_home`, over one of the same id: its desktop
/// entry, `applications/<id>.desktop`, and its icon, `icons/hicolor/scalable/
/// apps/<id>.svg` and a PNG at each of DESKTOP_ICON_SIZES. The entry is hidden
/// by the launcher once its program is gone. Returns the entry's path, or why
/// it couldn't be written.
Result<std::filesystem::path> create_desktop_entry(const DesktopEntry& entry, const std::filesystem::path& data_home);

/// `program` as an Exec line of a desktop entry names it: quoted when it holds
/// a space or another character the spec reserves, and escaped for a desktop
/// entry's string.
[[nodiscard]] std::string desktop_exec(const std::filesystem::path& program);

} // namespace ftk
