#include "core/project.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"
#include "platform/platform.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <fstream>

namespace fjell {

Project Project::create(const std::filesystem::path& directory,
                        const std::string& name) {
    auto root = directory / name;
    std::filesystem::create_directories(root);
    std::filesystem::create_directories(root / "assets");
    std::filesystem::create_directories(root / "scenes");
    std::filesystem::create_directories(root / "scripts");

    ProjectConfig config;
    config.name = name;

    Project project(root, config);
    project.save();

    // Initialise scripting support for the new project
    Project proj(root, config);
    refresh_headers(proj);
    write_scripts_cmake(proj);

    FJELL_CORE_INFO("Project created: {} at {}", name, root.string());
    return project;
}

std::optional<Project> Project::open(const std::filesystem::path& root) {
    auto manifest_path = root / MANIFEST_NAME;
    if (!std::filesystem::exists(manifest_path)) {
        FJELL_CORE_WARN("No {} found in {}", MANIFEST_NAME, root.string());
        return std::nullopt;
    }

    std::ifstream file(manifest_path);
    if (!file.is_open()) {
        FJELL_CORE_ERROR("Failed to open {}", manifest_path.string());
        return std::nullopt;
    }

    try {
        auto j = nlohmann::json::parse(file);
        ProjectConfig config;
        config.name = j.value("name", "Untitled");
        config.default_scene = j.value("default_scene", "scenes/main.fjell");
        config.layout = j.value("layout", "");

        FJELL_CORE_INFO("Project opened: {} ({})", config.name, root.string());
        return Project(root, config);
    } catch (const nlohmann::json::parse_error& e) {
        FJELL_CORE_ERROR("Failed to parse {}: {}", manifest_path.string(), e.what());
        return std::nullopt;
    }
}

Project::Project(std::filesystem::path root, ProjectConfig config)
    : root_{std::move(root)}, config_{std::move(config)} {}

std::string Project::resolve(const std::string& relative) const {
    return (root_ / relative).string();
}

void Project::save() const {
    auto path = root_ / MANIFEST_NAME;
    nlohmann::json j;
    j["name"] = config_.name;
    j["default_scene"] = config_.default_scene;
    if (!config_.layout.empty()) {
        j["layout"] = config_.layout;
    }

    std::ofstream file(path);
    if (!file.is_open()) {
        FJELL_CORE_ERROR("Failed to save project: {}", path.string());
        return;
    }
    file << j.dump(2);
    FJELL_CORE_INFO("Project saved: {}", path.string());
}

void Project::refresh_headers(const Project& /*project*/) {
    // SDK headers are included directly from engine source via
    // -I<engine>/src/scripting in the generated CMakeLists.txt.
    // No copying or symlinking needed.
}

void Project::write_scripts_cmake(const Project& project) {
    auto cmake_path = project.root() / "scripts" / "CMakeLists.txt";
    auto sdk_include = base_path::dir() / ".." / "src" / "scripting";
    write_scripts_cmake_to(cmake_path, sdk_include);
}

void Project::write_scripts_cmake_to(const std::filesystem::path& cmake_path,
                                     const std::filesystem::path& sdk_include) {
    namespace fs = std::filesystem;
    fs::create_directories(cmake_path.parent_path());

    auto sdk_dir = sdk_include.generic_string();
    // fjell.lib is generated next to fjell.exe by ENABLE_EXPORTS.
    // Use the exe's directory to find it reliably in both dev and install layouts.
    auto exe_dir = platform::executable_path().parent_path();
    auto lib_path = (exe_dir / "fjell.lib").generic_string();

    std::ofstream cmake_file(cmake_path);
    if (!cmake_file.is_open()) {
        FJELL_CORE_ERROR("Failed to write {}", cmake_path.string());
        return;
    }

    cmake_file
        << "cmake_minimum_required(VERSION 3.25)\n"
           "project(FjellGame LANGUAGES CXX)\n\n"
           "set(CMAKE_CXX_STANDARD 23)\n"
           "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n"
           "set(CMAKE_CXX_EXTENSIONS OFF)\n"
           "set(CMAKE_POSITION_INDEPENDENT_CODE ON)\n"
           "set(CMAKE_EXPORT_COMPILE_COMMANDS ON)\n\n"
           "# Symlink compile_commands.json to source dir for LSP\n"
           "file(CREATE_LINK\n"
           "    \"${CMAKE_BINARY_DIR}/compile_commands.json\"\n"
           "    \"${CMAKE_CURRENT_SOURCE_DIR}/compile_commands.json\"\n"
           "    SYMBOLIC)\n\n"
           "# GLM for math types used in SDK headers\n"
           "include(FetchContent)\n"
           "FetchContent_Declare(glm\n"
           "    GIT_REPOSITORY https://github.com/g-truc/glm.git\n"
           "    GIT_TAG 1.0.1\n"
           "    GIT_SHALLOW TRUE)\n"
           "FetchContent_MakeAvailable(glm)\n\n"
           "file(GLOB GAME_SOURCES \"${CMAKE_CURRENT_SOURCE_DIR}/*.cpp\")\n"
           "file(GLOB GENERATED_SOURCES \"${CMAKE_CURRENT_SOURCE_DIR}/generated/*.generated.cpp\")\n"
           "file(GLOB VERIFY_SOURCES \"${CMAKE_CURRENT_SOURCE_DIR}/generated/*.verify.cpp\")\n\n"
           "add_library(game SHARED ${GAME_SOURCES} ${GENERATED_SOURCES} ${VERIFY_SOURCES})\n\n"
        << "target_include_directories(game PRIVATE\n"
           "    " << sdk_dir << "\n"
           "    ${CMAKE_CURRENT_SOURCE_DIR}\n"
           ")\n\n"
           "target_link_libraries(game PRIVATE glm::glm)\n\n"
           "if(MSVC)\n"
           "    # Link against engine import library so plugin DLL resolves engine symbols\n"
           "    target_link_libraries(game PRIVATE \"" << lib_path << "\")\n"
           "    target_compile_options(game PRIVATE /W3)\n"
           "else()\n"
           "    target_compile_options(game PRIVATE -Wall -Wextra -Wno-invalid-offsetof -Wno-attributes -fno-gnu-unique)\n"
           "endif()\n\n"
           "set_target_properties(game PROPERTIES\n"
           "    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..\n"
           "    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..\n"
           "    PREFIX \"\"\n"
           ")\n"
           "# Prevent MSVC multi-config from adding Debug/Release subdirectories\n"
           "foreach(CFG ${CMAKE_CONFIGURATION_TYPES})\n"
           "    string(TOUPPER ${CFG} CFG_UPPER)\n"
           "    set_target_properties(game PROPERTIES\n"
           "        LIBRARY_OUTPUT_DIRECTORY_${CFG_UPPER} ${CMAKE_CURRENT_SOURCE_DIR}/..\n"
           "        RUNTIME_OUTPUT_DIRECTORY_${CFG_UPPER} ${CMAKE_CURRENT_SOURCE_DIR}/..\n"
           "    )\n"
           "endforeach()\n";
}

} // namespace fjell
