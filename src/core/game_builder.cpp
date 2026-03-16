#include "core/game_builder.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"
#include "core/project.hpp"

#include <nlohmann/json.hpp>

#include <unistd.h>

#include <array>
#include <fstream>

namespace fjell {

bool GameBuilder::build(const Project& project,
                        const std::filesystem::path& output_dir) {
    namespace fs = std::filesystem;

    try {
        fs::create_directories(output_dir);

        // Copy engine binary
        std::array<char, 4096> buf{};
        auto len = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
        if (len <= 0) {
            FJELL_CORE_ERROR("Failed to read /proc/self/exe");
            return false;
        }
        buf[len] = '\0';
        auto exe_path = fs::path(buf.data());
        auto dest_exe = output_dir / project.config().name;
        fs::copy_file(exe_path, dest_exe,
                      fs::copy_options::overwrite_existing);
        // Make executable
        fs::permissions(dest_exe, fs::perms::owner_exec | fs::perms::group_exec |
                        fs::perms::others_exec, fs::perm_options::add);

        // Copy shaders from engine
        auto engine_shaders = base_path::dir() / "shaders";
        auto dest_shaders = output_dir / "shaders";
        if (fs::exists(engine_shaders)) {
            fs::copy(engine_shaders, dest_shaders,
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        }

        // Copy assets from project
        auto project_assets = project.root() / "assets";
        auto dest_assets = output_dir / "assets";
        if (fs::exists(project_assets)) {
            fs::copy(project_assets, dest_assets,
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        }

        // Copy scenes from project
        auto project_scenes = project.root() / "scenes";
        auto dest_scenes = output_dir / "scenes";
        if (fs::exists(project_scenes)) {
            fs::copy(project_scenes, dest_scenes,
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        }

        // Copy game scripts .so if available
        auto game_so = project.root() / "game.so";
        if (fs::exists(game_so)) {
            fs::copy_file(game_so, output_dir / "game.so",
                          fs::copy_options::overwrite_existing);
        }

        // Write game.fjell (acts as both game mode marker and config)
        nlohmann::json j;
        j["name"] = project.config().name;
        j["version"] = project.config().version;
        j["default_scene"] = project.config().default_scene;
        j["window"] = {
            {"width", project.config().window.width},
            {"height", project.config().window.height},
            {"title", project.config().window.title}
        };

        std::ofstream game_file(output_dir / "game.fjell");
        game_file << j.dump(2);

        FJELL_CORE_INFO("Game built: {}", output_dir.string());
        return true;

    } catch (const fs::filesystem_error& e) {
        FJELL_CORE_ERROR("Build failed: {}", e.what());
        return false;
    }
}

} // namespace fjell
