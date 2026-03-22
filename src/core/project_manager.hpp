#pragma once

#include "core/delegate.hpp"
#include "core/project.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace fjell {

class Scene;
class ScriptManager;

/// Owns the active project and provides project lifecycle operations:
/// open/close, scene scanning, model scanning, script building.
class ProjectManager {
public:
    void open(const std::filesystem::path& path);
    Project create(const std::filesystem::path& directory, const std::string& name);
    void close();

    void scan_scenes();
    void scan_models();

    /// Build scripts: generate register_all.cpp, cmake configure + build.
    /// On success, calls scripts.reload().
    void build_scripts(ScriptManager& scripts, Scene& scene);

    /// Generate a new script .cpp file in the project's scripts/ directory.
    void create_script(const std::string& class_name);

    [[nodiscard]] Project* active() { return project_ ? &*project_ : nullptr; }
    [[nodiscard]] const std::vector<std::string>& scenes() const { return available_scenes_; }
    [[nodiscard]] const std::vector<std::string>& models() const { return available_models_; }

    // Events
    Delegate<void(const std::string&)> on_opened;   // project name
    Delegate<void()> on_closed;

private:
    std::optional<Project> project_;
    std::vector<std::string> available_scenes_;
    std::vector<std::string> available_models_;
};

} // namespace fjell
