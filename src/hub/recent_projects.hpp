#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace fjell::hub {

struct RecentProject {
    std::string path;
    std::string name;
};

class RecentProjects {
public:
    RecentProjects();

    void load();
    void save() const;
    void add(const std::string& path, const std::string& name);

    [[nodiscard]] const std::vector<RecentProject>& list() const { return projects_; }

private:
    [[nodiscard]] static std::filesystem::path config_path();

    std::vector<RecentProject> projects_;
};

} // namespace fjell::hub
