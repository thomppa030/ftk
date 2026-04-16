#pragma once

#include "core/asset_database.hpp"

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

    void save() const;

    // Re-copy engine scripting headers into the project's include/ dir
    static void refresh_headers(const Project& project);

    // Write/overwrite the engine-managed scripts/CMakeLists.txt
    static void write_scripts_cmake(const Project& project);

    // Write CMakeLists.txt to a specific path (safe to call from background thread)
    static void write_scripts_cmake_to(const std::filesystem::path& cmake_path,
                                       const std::filesystem::path& sdk_include);

    static constexpr const char* MANIFEST_NAME = "project.fjell";

private:
    Project(std::filesystem::path root, ProjectConfig config);

    std::filesystem::path root_;
    ProjectConfig config_;
    std::unique_ptr<AssetDatabase> asset_db_;
};

} // namespace fjell
