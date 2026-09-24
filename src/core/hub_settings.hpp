#pragma once

#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <string_view>

namespace fjell {

/// The file the hub and the editor keep what the user worked with lately
/// in: the recent projects and the file browser's recent folders.
/// %APPDATA%\fjell\hub.json on Windows, $XDG_CONFIG_HOME (or ~/.config)
/// /fjell/hub.json elsewhere.
[[nodiscard]] std::filesystem::path hub_settings_path();

/// One key of it (or of the file at `path`); null when the file or the key
/// is missing.
[[nodiscard]] nlohmann::json read_hub_setting(std::string_view key,
                                              const std::filesystem::path& path = hub_settings_path());

/// Writes one key, keeping the others as they are.
void write_hub_setting(std::string_view key, const nlohmann::json& value,
                       const std::filesystem::path& path = hub_settings_path());

} // namespace fjell
