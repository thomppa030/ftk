#include "core/project.hpp"
#include "core/asset_database.hpp"
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
    std::filesystem::create_directories(root / ".fjell");

    ProjectConfig config;
    config.name = name;

    Project project(root, config);
    project.asset_db_ = std::make_unique<AssetDatabase>(root);
    project.asset_remap_ = std::make_unique<AssetRemap>(root);
    project.save();

    write_gitignore(root);

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
        config.texture_import_dir = j.value("texture_import_dir", "assets/textures");
        if (j.contains("render")) {
            from_json(j["render"], config.render);
        }
        if (j.contains("network")) {
            const auto& n = j["network"];
            config.network.max_players = n.value("max_players", 16U);
            config.network.tick_rate   = n.value("tick_rate", 30U);
            config.network.port        = static_cast<uint16_t>(n.value("port", 7777U));
        }

        FJELL_CORE_INFO("Project opened: {} ({})", config.name, root.string());
        auto project = Project(root, config);

        // Projects created before the engine scaffolded one still need it.
        write_gitignore(root);

        // Initialize asset database
        project.asset_db_ = std::make_unique<AssetDatabase>(root);
        if (auto r = project.asset_db_->load(); !r) {
            FJELL_CORE_WARN("Asset database: {}", r.error());
        }
        project.asset_remap_ = std::make_unique<AssetRemap>(root);
        if (auto r = project.asset_remap_->load(); !r) {
            FJELL_CORE_WARN("Asset remap: {}", r.error());
        }

        project.asset_db_->rebuild();
        if (project.asset_db_->dirty()) {
            if (auto r = project.asset_db_->save(); !r) {
                FJELL_CORE_WARN("Asset database save: {}", r.error());
            } else {
                FJELL_CORE_INFO("Asset database: {} assets tracked", project.asset_db_->size());
            }
        }

        return project;
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

std::string Project::resolve(const AssetId& id) const {
    if (!asset_db_ || !id.valid()) return {};
    if (const auto* entry = asset_db_->find(id)) {
        return (root_ / entry->relative_path).string();
    }

    // The asset this id named is gone. A remap says what replaced it, which
    // catches references that were not rewritten when it went.
    if (asset_remap_) {
        if (auto to = asset_remap_->resolve(id); to.valid()) {
            if (const auto* entry = asset_db_->find(to)) {
                FJELL_CORE_DEBUG("Asset {} resolved through remap to {}",
                                 id.to_string(), to.to_string());
                return (root_ / entry->relative_path).string();
            }
        }
    }
    return {};
}

std::string Project::path_of(const AssetId& id) const {
    if (!asset_db_ || !id.valid()) return {};
    const auto* entry = asset_db_->find(id);
    return entry ? entry->relative_path : std::string{};
}

AssetId Project::id_of(const std::string& relative_path) const {
    if (!asset_db_) return {};
    const auto* entry = asset_db_->find_by_path(relative_path);
    return entry ? entry->id : AssetId{};
}

void Project::save() const {
    auto path = root_ / MANIFEST_NAME;
    nlohmann::json j;
    j["name"] = config_.name;
    j["default_scene"] = config_.default_scene;
    j["texture_import_dir"] = config_.texture_import_dir;
    if (!config_.layout.empty()) {
        j["layout"] = config_.layout;
    }
    to_json(j["render"], config_.render);

    j["network"] = {
        {"max_players", config_.network.max_players},
        {"tick_rate", config_.network.tick_rate},
        {"port", config_.network.port},
    };

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
    write_scripts_cmake_to(cmake_path, base_path::sdk_include());
}

void Project::write_gitignore(const std::filesystem::path& root) {
    auto path = root / ".gitignore";
    if (std::filesystem::exists(path)) return;

    std::ofstream file(path);
    if (!file.is_open()) {
        FJELL_CORE_ERROR("Failed to write {}", path.string());
        return;
    }

    // scripts/CMakeLists.txt and compile_commands.json are regenerated on every
    // build with absolute paths from the machine that built them — tracking them
    // means a permanently dirty tree.
    file << "# Script build output\n"
            "scripts/build/\n"
            "scripts/generated/\n"
            "scripts/register_all.cpp\n"
            "scripts/CMakeLists.txt\n"
            "scripts/compile_commands.json\n"
            "game.so\n"
            "game.dll\n"
            "game.pdb\n"
            "\n"
            "# Packaged builds\n"
            "build/\n"
            "\n"
            "# Editor state\n"
            ".fjell/\n"
            "imgui.ini\n"
            "Screenshots/\n";
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
           "# compile_commands.json where editors look for it. COPY_ON_ERROR because\n"
           "# symlinks need Developer Mode or elevation on Windows.\n"
           "file(CREATE_LINK\n"
           "    \"${CMAKE_BINARY_DIR}/compile_commands.json\"\n"
           "    \"${CMAKE_CURRENT_SOURCE_DIR}/compile_commands.json\"\n"
           "    SYMBOLIC COPY_ON_ERROR)\n\n"
           "# GLM for math types used in SDK headers\n"
           "include(FetchContent)\n"
           "FetchContent_Declare(glm\n"
           "    GIT_REPOSITORY https://github.com/g-truc/glm.git\n"
           "    GIT_TAG 1.0.1\n"
           "    GIT_SHALLOW TRUE)\n"
           "FetchContent_MakeAvailable(glm)\n\n"
           "# CONFIGURE_DEPENDS so a newly added script is picked up by a plain\n"
           "# build, without having to reconfigure by hand.\n"
           "file(GLOB GAME_SOURCES CONFIGURE_DEPENDS \"${CMAKE_CURRENT_SOURCE_DIR}/*.cpp\")\n"
           "file(GLOB GENERATED_SOURCES CONFIGURE_DEPENDS \"${CMAKE_CURRENT_SOURCE_DIR}/generated/*.generated.cpp\")\n"
           "file(GLOB VERIFY_SOURCES CONFIGURE_DEPENDS \"${CMAKE_CURRENT_SOURCE_DIR}/generated/*.verify.cpp\")\n\n"
           "add_library(game SHARED ${GAME_SOURCES} ${GENERATED_SOURCES} ${VERIFY_SOURCES})\n\n"
        << "target_include_directories(game PRIVATE\n"
           "    \"" << sdk_dir << "\"\n"
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
