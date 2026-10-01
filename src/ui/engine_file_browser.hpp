#pragma once

#include "core/hub_settings.hpp"
#include "ftk/app/file_browser.hpp"
#include "ui/engine_imgui_files.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace fjell {

/// The hub setting the file browsers' recent folders are kept under.
inline constexpr std::string_view HUB_RECENT_FOLDERS_KEY = "recent_folders";

/// Sets a file browser up the way every Fjell window has one: on `device`, with
/// the engine's ImGui files, and with the folders picked from lately kept in
/// the hub's settings, so the hub and every editor offer the same ones.
inline void set_up_file_browser(FileBrowser& browser, gpu::Device* device) {
    browser.set_device(device, engine_imgui_files());
    browser.set_recent_folders({
        .load = [] {
            std::vector<std::string> folders;
            if (const nlohmann::json list = read_hub_setting(HUB_RECENT_FOLDERS_KEY); list.is_array()) {
                for (const auto& folder : list) {
                    if (folder.is_string()) folders.push_back(folder.get<std::string>());
                }
            }
            return folders;
        },
        .save = [](const std::vector<std::string>& folders) { write_hub_setting(HUB_RECENT_FOLDERS_KEY, folders); },
    });
}

} // namespace fjell
