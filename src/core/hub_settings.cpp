#include "core/hub_settings.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
#include <string>

namespace fjell {

namespace {

nlohmann::json read_all(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) return nlohmann::json::object();
    auto j = nlohmann::json::parse(file, nullptr, false);
    return j.is_object() ? j : nlohmann::json::object();
}

} // namespace

std::filesystem::path hub_settings_path() {
    std::filesystem::path base;
#ifdef _WIN32
    const char* appdata = std::getenv("APPDATA");
    base = appdata != nullptr && appdata[0] != '\0' ? std::filesystem::path(appdata)
                                                    : std::filesystem::temp_directory_path();
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg != nullptr && xdg[0] != '\0') {
        base = xdg;
    } else {
        const char* home = std::getenv("HOME");
        base = home != nullptr ? std::filesystem::path(home) / ".config" : std::filesystem::temp_directory_path();
    }
#endif
    return base / "fjell" / "hub.json";
}

nlohmann::json read_hub_setting(std::string_view key, const std::filesystem::path& path) {
    const nlohmann::json all = read_all(path);
    const auto it = all.find(std::string(key));
    return it != all.end() ? *it : nlohmann::json();
}

void write_hub_setting(std::string_view key, const nlohmann::json& value, const std::filesystem::path& path) {
    nlohmann::json all = read_all(path);
    all[std::string(key)] = value;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path);
    if (file.is_open()) file << all.dump(2);
}

} // namespace fjell
