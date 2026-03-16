#include "hub/recent_projects.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>

namespace fjell::hub {

RecentProjects::RecentProjects() {
    load();
}

std::filesystem::path RecentProjects::config_path() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    std::filesystem::path base;
    if (xdg && xdg[0] != '\0') {
        base = xdg;
    } else {
        const char* home = std::getenv("HOME");
        base = home ? std::filesystem::path(home) / ".config"
                    : std::filesystem::path("/tmp");
    }
    return base / "fjell" / "hub.json";
}

void RecentProjects::load() {
    projects_.clear();
    auto path = config_path();
    if (!std::filesystem::exists(path)) return;

    std::ifstream file(path);
    if (!file.is_open()) return;

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(file);
    } catch (...) {
        return;
    }

    if (!j.contains("recent_projects")) return;

    for (const auto& entry : j["recent_projects"]) {
        RecentProject p;
        p.path = entry.value("path", "");
        p.name = entry.value("name", "");
        if (!p.path.empty()) {
            projects_.push_back(std::move(p));
        }
    }
}

void RecentProjects::save() const {
    auto path = config_path();
    std::filesystem::create_directories(path.parent_path());

    nlohmann::json j;
    j["recent_projects"] = nlohmann::json::array();
    for (const auto& p : projects_) {
        j["recent_projects"].push_back({{"path", p.path}, {"name", p.name}});
    }

    std::ofstream file(path);
    if (file.is_open()) {
        file << j.dump(2);
    }
}

void RecentProjects::add(const std::string& path, const std::string& name) {
    // Remove existing entry with same path
    std::erase_if(projects_, [&](const RecentProject& p) { return p.path == path; });

    // Insert at front
    projects_.insert(projects_.begin(), {path, name});

    // Keep max 10
    if (projects_.size() > 10) {
        projects_.resize(10);
    }

    save();
}

} // namespace fjell::hub
