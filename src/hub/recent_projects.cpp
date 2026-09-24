#include "hub/recent_projects.hpp"

#include "core/hub_settings.hpp"

#include <nlohmann/json.hpp>

namespace fjell::hub {

RecentProjects::RecentProjects() {
    load();
}

void RecentProjects::load() {
    projects_.clear();
    const nlohmann::json list = read_hub_setting("recent_projects");
    if (!list.is_array()) return;
    for (const auto& entry : list) {
        RecentProject p;
        p.path = entry.value("path", "");
        p.name = entry.value("name", "");
        if (!p.path.empty()) projects_.push_back(std::move(p));
    }
}

void RecentProjects::save() const {
    nlohmann::json list = nlohmann::json::array();
    for (const auto& p : projects_) list.push_back({{"path", p.path}, {"name", p.name}});
    write_hub_setting("recent_projects", list);
}

void RecentProjects::add(const std::string& path, const std::string& name) {
    // Always store absolute paths so the hub works regardless of CWD
    auto abs_path = std::filesystem::absolute(path).string();

    // Remove existing entry with same path
    std::erase_if(projects_, [&](const RecentProject& p) { return p.path == abs_path; });

    // Insert at front
    projects_.insert(projects_.begin(), {abs_path, name});

    // Keep max 10
    if (projects_.size() > 10) {
        projects_.resize(10);
    }

    save();
}

void RecentProjects::remove(const std::string& path) {
    std::erase_if(projects_, [&](const RecentProject& p) { return p.path == path; });
    save();
}

} // namespace fjell::hub
