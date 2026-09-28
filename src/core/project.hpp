#pragma once

#include "core/asset_id.hpp"

#include "core/asset_database.hpp"
#include "core/asset_remap.hpp"
#include "core/fixed_timestep.hpp"
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

    struct SimulationConfig {
        // How many fixed steps the simulation takes per second of game time:
        // physics, scripts' fixed_tick and the environment's simulated half
        // all run at this rate, and drawing blends between their steps. Read
        // when play starts; a session keeps its rate to the end.
        uint32_t fixed_tick_rate{FixedTimestep::DEFAULT_TICK_RATE};
        // How many of the nodes that leave the scene during play are
        // remembered by name, the last ones to go: what a report names when a
        // script uses a Ref whose node is gone. Read when play starts.
        uint32_t gone_nodes_remembered{1024};
        static constexpr uint32_t MAX_GONE_NODES_REMEMBERED = 65536;

        bool operator==(const SimulationConfig&) const = default;
    } simulation;

    struct PlayConfig {
        // Whether F11 (during play) leaves the docked viewport for a
        // fullscreen play view. Persisted per project so the preference
        // survives a restart the same way the dockspace layout choice does.
        bool fullscreen{false};
    } play;

    struct EnvironmentConfig {
        // How fast surfaces take on and shed the weather, in seconds to get
        // most of the way there. The same for every weather event.
        float wetting_seconds{60.0f};
        float drying_seconds{600.0f};
        float snowing_seconds{300.0f};
        float melting_seconds{1200.0f};

        bool operator==(const EnvironmentConfig&) const = default;
    } environment;

    struct UiConfig {
        // The resolution game UI is designed at. The UI editor previews at
        // this size, and screen-space widgets scale so a viewport of any
        // size shows the same proportions, unless a placement opts out. A
        // designer working in pixels against this frame gets a reactive HUD
        // without writing a single relative unit.
        uint32_t design_width{1920};
        uint32_t design_height{1080};

        bool operator==(const UiConfig&) const = default;
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

    /// Writes project.fjell, which then matches config().
    void save();

    /// The config as project.fjell holds it: as opened or last saved. What
    /// config() differs from it by is unsaved.
    [[nodiscard]] const ProjectConfig& saved_config() const { return saved_config_; }

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
    ProjectConfig saved_config_;
    std::unique_ptr<AssetDatabase> asset_db_;
    std::unique_ptr<AssetRemap> asset_remap_;
};

} // namespace fjell
