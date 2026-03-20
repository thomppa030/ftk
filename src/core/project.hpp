#pragma once

#include <cstdint>
#include <filesystem>
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

    void save() const;

    // Re-copy engine scripting headers into the project's include/ dir
    static void refresh_headers(const Project& project);

    // Write/overwrite the engine-managed scripts/CMakeLists.txt
    static void write_scripts_cmake(const Project& project);

    static constexpr const char* MANIFEST_NAME = "project.fjell";

private:
    Project(std::filesystem::path root, ProjectConfig config);

    std::filesystem::path root_;
    ProjectConfig config_;
};

} // namespace fjell
