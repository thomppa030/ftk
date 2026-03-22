#include "core/project_manager.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"
#include "platform/platform.hpp"
#include "scene/scene.hpp"
#include "scripting/script_manager.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fjell {

void ProjectManager::open(const std::filesystem::path& path) {
    auto proj = Project::open(path);
    if (!proj) return;
    project_ = std::move(proj);
    scan_models();
    scan_scenes();
    on_opened.broadcast(project_->config().name);
}

Project ProjectManager::create(const std::filesystem::path& directory,
                                const std::string& name) {
    auto proj = Project::create(directory, name);
    project_ = proj;
    scan_models();
    scan_scenes();
    on_opened.broadcast(project_->config().name);
    return proj;
}

void ProjectManager::close() {
    project_.reset();
    available_scenes_.clear();
    available_models_.clear();
    on_closed.broadcast();
}

void ProjectManager::scan_scenes() {
    available_scenes_.clear();
    if (!project_) return;

    auto scenes_dir = std::filesystem::path(project_->resolve("scenes"));
    if (!std::filesystem::exists(scenes_dir)) return;

    for (const auto& entry : std::filesystem::directory_iterator(scenes_dir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() == ".fjell") {
            available_scenes_.push_back(entry.path().string());
        }
    }
    std::sort(available_scenes_.begin(), available_scenes_.end());
}

void ProjectManager::scan_models() {
    available_models_.clear();
    auto* proj = active();
    const auto models_dir = std::filesystem::path(
        base_path::resolve_asset("assets/models", proj));
    if (!std::filesystem::exists(models_dir)) return;

    auto root = proj ? proj->root() : base_path::dir();

    for (const auto& entry : std::filesystem::directory_iterator(models_dir)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        if (ext == ".gltf" || ext == ".glb") {
            auto relative = std::filesystem::relative(entry.path(), root);
            available_models_.push_back(relative.string());
        }
    }
}

void ProjectManager::build_scripts(ScriptManager& scripts, Scene& scene) {
    if (!project_) return;

    Project::refresh_headers(*project_);

    auto scripts_dir = project_->root() / "scripts";
    auto build_dir = scripts_dir / "build";

    // Auto-generate register_all.cpp
    {
        std::ofstream reg(scripts_dir / "register_all.cpp");
        if (reg.is_open()) {
            reg << "// Auto-generated -- do not edit.\n";
            reg << "#include \"fjell/registry.hpp\"\n\n";
            reg << "FJELL_REGISTER()\n";
        }
    }

    Project::refresh_headers(*project_);
    Project::write_scripts_cmake(*project_);

    auto cmd = "cmake -B " + build_dir.string() + " -S " + scripts_dir.string() +
               " -DCMAKE_BUILD_TYPE=Release 2>&1 && "
               "cmake --build " + build_dir.string() + " -j$(nproc) 2>&1";

    FJELL_CORE_INFO("Building scripts...");

    std::string build_output;
    int ret = platform::run_command(cmd, build_output);
    bool success = (ret == 0);

    std::istringstream iss(build_output);
    std::string s;
    while (std::getline(iss, s)) {
        while (!s.empty() && (s.back() == '\r')) s.pop_back();
        if (s.empty()) continue;
        if (s.find("error") != std::string::npos) {
            FJELL_CORE_ERROR("[build] {}", s);
        } else if (s.find("warning") != std::string::npos) {
            FJELL_CORE_WARN("[build] {}", s);
        } else {
            FJELL_CORE_INFO("[build] {}", s);
        }
    }

    if (success) {
        FJELL_CORE_INFO("Scripts built successfully");
        auto so_path = project_->root() / "game.so";
        if (std::filesystem::exists(so_path)) {
            scripts.reload(scene, so_path);
        }
    } else {
        FJELL_CORE_ERROR("Script build failed");
    }
}

void ProjectManager::create_script(const std::string& class_name) {
    if (!project_) return;

    std::string filename = class_name;
    for (auto& c : filename)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    auto script_dir = project_->root() / "scripts";
    auto path = script_dir / (filename + ".cpp");

    std::ofstream file(path);
    if (file.is_open()) {
        file << "#include \"fjell/registry.hpp\"\n"
             << "#include \"fjell/world.hpp\"\n"
             << "#include <array>\n"
             << "#include <cmath>\n"
             << "\n"
             << "class " << class_name << " : public fjell::ScriptComponent {\n"
             << "  FJELL_COMPONENT(" << class_name << ")\n"
             << "\n"
             << "  static auto reflect() {\n"
             << "    return std::array{\n"
             << "    };\n"
             << "  }\n"
             << "\n"
             << "  void begin_play() override {\n"
             << "  }\n"
             << "\n"
             << "  void tick(float dt) override {\n"
             << "    (void)dt;\n"
             << "  }\n"
             << "\n"
             << "private:\n"
             << "};\n";

        FJELL_CORE_INFO("Created script: {}", path.string());
    } else {
        FJELL_CORE_ERROR("Failed to create script: {}", path.string());
    }
}

} // namespace fjell
