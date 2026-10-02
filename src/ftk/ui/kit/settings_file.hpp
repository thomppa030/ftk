#pragma once

// Settings kept in the UI's settings file, the one beside the program that
// holds where the panels were left: how this machine likes something
// shown, never part of a document or a project. A section is read back
// line by line when the file is loaded, and written whole when it is saved.
//
//     [FjellView][Editor]
//     HierarchyTextSize=15
//
//     ui::add_settings_section({
//         .name = "FjellView",
//         .read = [](std::string_view entry, std::string_view line) { ... },
//         .write = [] { return std::vector<ui::SettingsEntry>{{"Editor", {"HierarchyTextSize=15"}}}; },
//     });

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ftk::ui {

/// An entry of a section: [section][name], then its lines.
struct SettingsEntry {
    std::string name;
    /// Each "Key=value".
    std::vector<std::string> lines;
};

struct SettingsSection {
    /// Its name in the file.
    std::string name{};
    /// A line of the entry `entry`, as the file is read.
    std::function<void(std::string_view entry, std::string_view line)> read{};
    /// The entries to write, as the file is saved.
    std::function<std::vector<SettingsEntry>()> write{};
};

/// Keeps `section` in the settings file. Before the file is first read,
/// for the section to be read back; a second section of the same name
/// takes the first one's place.
void add_settings_section(SettingsSection section);

/// A setting kept in a section changed: the file is saved soon after.
void settings_changed();

} // namespace ftk::ui
