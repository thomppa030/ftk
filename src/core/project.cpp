#include "core/project.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#include <nlohmann/json.hpp>

#include <fstream>

namespace fjell {

Project::Project(std::filesystem::path root, ProjectConfig config)
    : root_{std::move(root)}, config_{std::move(config)} {}

Project Project::create(const std::filesystem::path& directory,
                        const std::string& name) {
    namespace fs = std::filesystem;

    auto root = fs::absolute(directory);
    fs::create_directories(root / "assets" / "textures");
    fs::create_directories(root / "assets" / "models");
    fs::create_directories(root / "scenes");
    fs::create_directories(root / "scripts");
    fs::create_directories(root / "include");


    ProjectConfig config;
    config.name = name;
    config.window.title = name;

    Project project{root, std::move(config)};
    project.save();

    // Write a default empty scene
    nlohmann::json scene_json;
    scene_json["objects"] = nlohmann::json::array();
    std::ofstream scene_file(root / "scenes" / "main.fjell");
    scene_file << scene_json.dump(2);

    // Copy scripting headers for script compilation
    Project proj(root, config);
    refresh_headers(proj);
    write_scripts_cmake(proj);

    FJELL_CORE_INFO("Project created: {} at {}", name, root.string());
    return project;
}

std::optional<Project> Project::open(const std::filesystem::path& directory) {
    namespace fs = std::filesystem;

    auto root = fs::absolute(directory);
    auto manifest_path = root / MANIFEST_NAME;

    if (!fs::exists(manifest_path)) {
        FJELL_CORE_WARN("No {} found in {}", MANIFEST_NAME, root.string());
        return std::nullopt;
    }

    std::ifstream file(manifest_path);
    if (!file.is_open()) {
        FJELL_CORE_ERROR("Failed to open {}", manifest_path.string());
        return std::nullopt;
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(file);
    } catch (const nlohmann::json::parse_error& e) {
        FJELL_CORE_ERROR("Failed to parse {}: {}", manifest_path.string(), e.what());
        return std::nullopt;
    }

    ProjectConfig config;
    config.name = j.value("name", "Untitled");
    config.version = j.value("version", "0.1.0");
    config.default_scene = j.value("default_scene", "scenes/main.fjell");
    config.layout = j.value("layout", "");

    if (j.contains("window")) {
        auto& w = j["window"];
        config.window.width = w.value("width", 1280u);
        config.window.height = w.value("height", 720u);
        config.window.title = w.value("title", config.name);
    }

    FJELL_CORE_INFO("Project opened: {} ({})", config.name, root.string());
    return Project{root, std::move(config)};
}

std::string Project::resolve(const std::string& relative) const {
    return (root_ / relative).string();
}

void Project::save() const {
    nlohmann::json j;
    j["name"] = config_.name;
    j["version"] = config_.version;
    j["default_scene"] = config_.default_scene;
    if (!config_.layout.empty()) j["layout"] = config_.layout;
    j["window"] = {
        {"width", config_.window.width},
        {"height", config_.window.height},
        {"title", config_.window.title}
    };

    auto path = root_ / MANIFEST_NAME;
    std::ofstream file(path);
    if (!file.is_open()) {
        FJELL_CORE_ERROR("Failed to save project: {}", path.string());
        return;
    }
    file << j.dump(2);
    FJELL_CORE_INFO("Project saved: {}", path.string());
}

void Project::refresh_headers(const Project& project) {
    namespace fs = std::filesystem;
    const auto& root = project.root();

    // Source from the staged SDK directory (build/sdk/fjell/)
    auto sdk_dir = base_path::dir() / "sdk" / "fjell";
    if (!fs::is_directory(sdk_dir)) {
        FJELL_CORE_WARN("SDK directory not found: {}", sdk_dir.string());
        return;
    }

    // Clean old layout dirs from previous versions
    auto include_dir = root / "include";
    for (const auto& old_dir : {"scripting", "scene", "core"}) {
        auto old_path = include_dir / old_dir;
        if (fs::is_directory(old_path)) {
            fs::remove_all(old_path);
        }
    }

    // Remove + recreate include/fjell/ to clean stale files
    auto dst_dir = include_dir / "fjell";
    if (fs::is_directory(dst_dir)) {
        fs::remove_all(dst_dir);
    }
    fs::create_directories(dst_dir);

    // Copy all files from sdk/fjell/ to include/fjell/
    for (const auto& entry : fs::directory_iterator(sdk_dir)) {
        if (entry.is_regular_file()) {
            fs::copy_file(entry.path(), dst_dir / entry.path().filename(),
                          fs::copy_options::overwrite_existing);
        }
    }

    // Migrate old include paths in project scripts
    auto scripts_dir = root / "scripts";
    if (fs::is_directory(scripts_dir)) {
        for (const auto& entry : fs::directory_iterator(scripts_dir)) {
            if (!entry.is_regular_file()) continue;
            auto ext = entry.path().extension().string();
            if (ext != ".cpp" && ext != ".hpp" && ext != ".h") continue;

            std::ifstream in(entry.path());
            std::string content((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
            in.close();

            std::string updated = content;
            // "scripting/fjell_X.hpp" → "fjell/X.hpp"
            // "scripting/fjell_X.h"   → "fjell/X.h"
            // "scene/transform.hpp"   → "fjell/transform.hpp"
            // "core/input.hpp"        → "fjell/input.hpp"
            const std::pair<std::string, std::string> replacements[] = {
                {"\"scripting/fjell_api.h\"",          "\"fjell/api.h\""},
                {"\"scripting/fjell_actor.hpp\"",      "\"fjell/actor.hpp\""},
                {"\"scripting/fjell_component.hpp\"",  "\"fjell/component.hpp\""},
                {"\"scripting/fjell_world.hpp\"",      "\"fjell/world.hpp\""},
                {"\"scripting/fjell_registry.hpp\"",   "\"fjell/registry.hpp\""},
                {"\"scene/transform.hpp\"",            "\"fjell/transform.hpp\""},
                {"\"core/input.hpp\"",                 "\"fjell/input.hpp\""},
            };
            for (const auto& [old_inc, new_inc] : replacements) {
                std::string::size_type pos = 0;
                while ((pos = updated.find(old_inc, pos)) != std::string::npos) {
                    updated.replace(pos, old_inc.size(), new_inc);
                    pos += new_inc.size();
                }
            }

            if (updated != content) {
                std::ofstream out(entry.path());
                out << updated;
                FJELL_CORE_INFO("Migrated includes: {}", entry.path().filename().string());
            }
        }
    }
}

void Project::write_scripts_cmake(const Project& project) {
    namespace fs = std::filesystem;
    auto scripts_cmake = project.root() / "scripts" / "CMakeLists.txt";
    fs::create_directories(scripts_cmake.parent_path());

    std::ofstream cmake_file(scripts_cmake);
    cmake_file << "cmake_minimum_required(VERSION 3.25)\n"
               << "project(FjellGame LANGUAGES CXX)\n\n"
               << "set(CMAKE_CXX_STANDARD 23)\n"
               << "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n"
               << "set(CMAKE_CXX_EXTENSIONS OFF)\n"
               << "set(CMAKE_POSITION_INDEPENDENT_CODE ON)\n"
               << "set(CMAKE_EXPORT_COMPILE_COMMANDS ON)\n\n"
               << "# Symlink compile_commands.json to source dir for LSP\n"
               << "file(CREATE_LINK\n"
               << "    \"${CMAKE_BINARY_DIR}/compile_commands.json\"\n"
               << "    \"${CMAKE_CURRENT_SOURCE_DIR}/compile_commands.json\"\n"
               << "    SYMBOLIC)\n\n"
               << "# GLM for math types used in SDK headers\n"
               << "include(FetchContent)\n"
               << "FetchContent_Declare(glm\n"
               << "    GIT_REPOSITORY https://github.com/g-truc/glm.git\n"
               << "    GIT_TAG 1.0.1\n"
               << "    GIT_SHALLOW TRUE)\n"
               << "FetchContent_MakeAvailable(glm)\n\n"
               << "file(GLOB GAME_SOURCES \"${CMAKE_CURRENT_SOURCE_DIR}/*.cpp\")\n\n"
               << "add_library(game SHARED ${GAME_SOURCES})\n\n"
               << "target_include_directories(game PRIVATE\n"
               << "    ${CMAKE_CURRENT_SOURCE_DIR}/../include\n"
               << ")\n\n"
               << "target_link_libraries(game PRIVATE glm::glm)\n\n"
               << "target_compile_options(game PRIVATE -Wall -Wextra -Wno-invalid-offsetof)\n\n"
               << "set_target_properties(game PROPERTIES\n"
               << "    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..\n"
               << "    PREFIX \"\"\n"
               << ")\n";
}

} // namespace fjell
