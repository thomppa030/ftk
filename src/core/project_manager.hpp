#pragma once

#include "core/delegate.hpp"
#include "core/project.hpp"

#ifndef NDEBUG
#include "platform/file_watcher.hpp"
#endif

#include <filesystem>
#include <future>
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

    /// Kick off an async script build (codegen + cmake). Non-blocking.
    /// Call poll_build() each frame to check for completion and trigger reload.
    void build_scripts(ScriptManager& scripts, Scene& scene);

    /// Check if the background build finished. If it succeeded, reload scripts.
    /// If playing is true, also calls begin_play after reload.
    /// Call once per frame from the main thread.
    void poll_build(ScriptManager& scripts, Scene& scene, bool playing);

    /// Generate a new script .cpp file in the project's scripts/ directory.
    void create_script(const std::string& class_name);

    [[nodiscard]] Project* active() { return project_ ? &*project_ : nullptr; }
    [[nodiscard]] const std::vector<std::string>& scenes() const { return available_scenes_; }
    [[nodiscard]] const std::vector<std::string>& models() const { return available_models_; }
    [[nodiscard]] bool is_building() const { return build_future_.valid(); }

    // Events
    Delegate<void(const std::string&)> on_opened;   // project name
    Delegate<void()> on_closed;

private:
    std::optional<Project> project_;
    std::vector<std::string> available_scenes_;
    std::vector<std::string> available_models_;

    // ── Async build state ────────────────────────────────────────────────
    struct BuildResult {
        bool success{false};
        std::string output;
        std::filesystem::path so_path;
    };
    std::future<BuildResult> build_future_;

#ifndef NDEBUG
    // ── Source file watcher (auto-build on .cpp/.hpp change) ─────────────
    platform::FileWatcher script_watcher_;
    bool source_changed_{false};
    std::chrono::steady_clock::time_point last_source_change_{};
    void setup_script_watcher();
#endif
};

} // namespace fjell
