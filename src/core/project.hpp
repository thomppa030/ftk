#pragma once

#include "core/asset_id.hpp"

#include "core/asset_database.hpp"
#include "core/asset_remap.hpp"
#include "renderer/render_settings.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace fjell {

struct ProjectConfig {
    std::string name;
    std::string version{"0.1.0"};

    struct WindowConfig {
        uint32_t width{1280};
        uint32_t height{720};
        std::string title{"Fjell Game"};
    } window;

    std::string default_scene{"scenes/main.fjell"};
    std::string layout;

    /// Where the importer writes textures it extracts from source models,
    /// relative to the project root. Each model gets a subfolder.
    std::string texture_import_dir{"assets/textures"};

    RenderSettings render;

    struct NetworkConfig {
        uint32_t max_players{16};
        uint32_t tick_rate{30};  // state-update send rate, Hz
        uint16_t port{7777};
    } network;

    struct PlayConfig {
        // Whether F11 (during play) leaves the docked viewport for a
        // fullscreen play view. Persisted per project so the preference
        // survives a restart the same way the dockspace layout choice does.
        bool fullscreen{false};
    } play;

    struct UiConfig {
        // The resolution game UI is designed at. The UI editor previews at
        // this size, and screen-space widgets scale so a viewport of any
        // size shows the same proportions, unless a placement opts out. A
        // designer working in pixels against this frame gets a reactive HUD
        // without writing a single relative unit.
        uint32_t design_width{1920};
        uint32_t design_height{1080};
    } ui;
};

class Project {
public:
    static Project create(const std::filesystem::path& directory,
                          const std::string& name);
    static std::optional<Project> open(const std::filesystem::path& directory);

    [[nodiscard]] std::string resolve(const std::string& relative) const;
    [[nodiscard]] const std::filesystem::path& root() const { return root_; }
    [[nodiscard]] const ProjectConfig& config() const { return config_; }
    [[nodiscard]] ProjectConfig& config() { return config_; }
    [[nodiscard]] AssetDatabase* asset_db() { return asset_db_.get(); }
    [[nodiscard]] const AssetDatabase* asset_db() const { return asset_db_.get(); }

    [[nodiscard]] AssetRemap* asset_remap() { return asset_remap_.get(); }
    [[nodiscard]] const AssetRemap* asset_remap() const { return asset_remap_.get(); }

    /// Absolute path of the asset with this id. Empty when the id is unknown,
    /// which is a dangling reference rather than something to paper over.
    [[nodiscard]] std::string resolve(const AssetId& id) const;

    /// Id of the asset at this project-relative path, invalid if untracked.
    [[nodiscard]] AssetId id_of(const std::string& relative_path) const;

    /// Project-relative path of the asset with this id, empty if unknown.
    /// For display — resolve() gives the absolute path for loading.
    [[nodiscard]] std::string path_of(const AssetId& id) const;

    void save() const;

    // Re-copy engine scripting headers into the project's include/ dir
    static void refresh_headers(const Project& project);

    // Write/overwrite the engine-managed scripts/CMakeLists.txt
    static void write_scripts_cmake(const Project& project);

    // Write CMakeLists.txt to a specific path (safe to call from background thread)
    static void write_scripts_cmake_to(const std::filesystem::path& cmake_path,
                                       const std::filesystem::path& sdk_include);

    // Write the default .gitignore, unless the project already has one.
    // Scaffolding, not generated config — a project's own additions survive.
    static void write_gitignore(const std::filesystem::path& root);

    static constexpr const char* MANIFEST_NAME = "project.fjell";

private:
    Project(std::filesystem::path root, ProjectConfig config);

    std::filesystem::path root_;
    ProjectConfig config_;
    std::unique_ptr<AssetDatabase> asset_db_;
    std::unique_ptr<AssetRemap> asset_remap_;
};

} // namespace fjell
