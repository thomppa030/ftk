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

    // Copy default texture from engine
    auto engine_texture = base_path::resolve("assets/textures/test.png");
    auto project_texture = root / "assets" / "textures" / "test.png";
    if (fs::exists(engine_texture) && !fs::exists(project_texture)) {
        fs::copy_file(engine_texture, project_texture);
    }

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
    fs::create_directories(root / "include" / "scripting");
    fs::create_directories(root / "include" / "scene");
    fs::create_directories(root / "include" / "core");

    auto copy_header = [&](const std::string& src, const std::string& dst) {
        auto from = base_path::resolve(src);
        auto to = root / "include" / dst;
        if (fs::exists(from)) {
            fs::copy_file(from, to, fs::copy_options::overwrite_existing);
        }
    };

    copy_header("scripting/fjell_api.h", "scripting/fjell_api.h");
    copy_header("scripting/fjell_actor.hpp", "scripting/fjell_actor.hpp");
    copy_header("scripting/fjell_component.hpp", "scripting/fjell_component.hpp");
    copy_header("scripting/fjell_world.hpp", "scripting/fjell_world.hpp");
    copy_header("scripting/fjell_registry.hpp", "scripting/fjell_registry.hpp");
    copy_header("scene/transform.hpp", "scene/transform.hpp");
    copy_header("core/input.hpp", "core/input.hpp");

    // Scaffold scripts CMakeLists.txt
    auto scripts_cmake = root / "scripts" / "CMakeLists.txt";
    if (!fs::exists(scripts_cmake)) {
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
                   << "file(GLOB GAME_SOURCES \"${CMAKE_CURRENT_SOURCE_DIR}/*.cpp\")\n\n"
                   << "add_library(game SHARED ${GAME_SOURCES})\n\n"
                   << "target_include_directories(game PRIVATE\n"
                   << "    ${CMAKE_CURRENT_SOURCE_DIR}/../include\n"
                   << ")\n\n"
                   << "target_compile_options(game PRIVATE -Wall -Wextra -Wno-invalid-offsetof)\n\n"
                   << "set_target_properties(game PROPERTIES\n"
                   << "    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..\n"
                   << "    PREFIX \"\"\n"
                   << ")\n";
    }

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

} // namespace fjell
