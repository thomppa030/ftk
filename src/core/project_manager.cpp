#include "core/project_manager.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"
#include "platform/platform.hpp"
#include "scene/scene.hpp"
#include "scripting/script_manager.hpp"

#include <nlohmann/json.hpp>

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
    auto generated_dir = scripts_dir / "generated";

    Project::refresh_headers(*project_);
    Project::write_scripts_cmake(*project_);

    // Run fjreflect codegen
    namespace fs = std::filesystem;
    fs::create_directories(generated_dir);

    auto fjreflect_path = base_path::dir() / "tools" / "fjreflect" / "fjreflect";
    if (!fs::exists(fjreflect_path)) {
        // Try alongside the engine binary
        fjreflect_path = base_path::dir() / "fjreflect";
    }

    bool has_codegen = false;
    if (fs::exists(fjreflect_path)) {
        // Collect script headers for codegen (class definitions live in .hpp)
        std::string source_list;
        for (const auto& entry : fs::directory_iterator(scripts_dir)) {
            if (!entry.is_regular_file()) continue;
            auto name = entry.path().filename().string();
            if (!name.ends_with(".hpp")) continue;
            source_list += " " + entry.path().string();
        }

        if (!source_list.empty()) {
            auto include_dir = project_->root() / "include";
            // Find GLM in the build dir (fetched by the game project CMake)
            auto glm_dir = build_dir / "_deps" / "glm-src";

            auto reflect_cmd = fjreflect_path.string() +
                " --output-dir " + generated_dir.string() +
                " --include-dir " + include_dir.string() +
                " --compiler g++";

            // Add GLM include if available
            if (fs::exists(glm_dir)) {
                reflect_cmd += " --include-dir " + glm_dir.string();
            }

            reflect_cmd += source_list + " 2>&1";

            std::string reflect_output;
            int reflect_ret = platform::run_command(reflect_cmd, reflect_output);
            if (reflect_ret != 0) {
                FJELL_CORE_ERROR("[fjreflect] codegen failed:");
                std::istringstream riss(reflect_output);
                std::string rline;
                while (std::getline(riss, rline)) {
                    FJELL_CORE_ERROR("[fjreflect] {}", rline);
                }
            } else {
                // Log output
                std::istringstream riss(reflect_output);
                std::string rline;
                while (std::getline(riss, rline)) {
                    if (!rline.empty()) FJELL_CORE_INFO("[fjreflect] {}", rline);
                }
                has_codegen = true;
            }
        }
    } else {
        FJELL_CORE_WARN("fjreflect not found — skipping codegen");
    }

    // Generate register_all.cpp — wire in codegen manifest if available
    {
        std::ofstream reg(scripts_dir / "register_all.cpp");
        if (reg.is_open()) {
            reg << "// Auto-generated -- do not edit.\n";
            reg << "#include \"fjell/registry.hpp\"\n\n";

            // Read manifest and generate extern declarations
            auto manifest_path = generated_dir / "manifest.json";
            if (has_codegen && fs::exists(manifest_path)) {
                std::ifstream mf(manifest_path);
                auto manifest = nlohmann::json::parse(mf, nullptr, false);
                if (manifest.is_object() && manifest.contains("types")) {
                    // Extern declarations for generated symbols
                    for (const auto& t : manifest["types"]) {
                        auto name = t["name"].get<std::string>();
                        auto fields_sym = t["fields_symbol"].get<std::string>();
                        reg << "extern \"C\" const FjellFieldDesc " << fields_sym << "[];\n";
                        reg << "extern \"C\" const uint32_t fjreflect_" << name << "_field_count;\n";
                        if (t.contains("trace_symbol") && !t["trace_symbol"].is_null()) {
                            reg << "extern \"C\" void " << t["trace_symbol"].get<std::string>()
                                << "(void*, void*);\n";
                        }
                    }

                    // Generated info table
                    reg << "\nstatic const FjellGeneratedInfo fjreflect_generated[] = {\n";
                    for (const auto& t : manifest["types"]) {
                        auto name = t["name"].get<std::string>();
                        auto fields_sym = t["fields_symbol"].get<std::string>();
                        auto count = t["field_count"].get<uint32_t>();
                        reg << "    {\"" << name << "\", " << fields_sym
                            << ", " << count << ", ";
                        if (t.contains("trace_symbol") && !t["trace_symbol"].is_null()) {
                            reg << t["trace_symbol"].get<std::string>();
                        } else {
                            reg << "nullptr";
                        }
                        reg << "},\n";
                    }
                    reg << "};\n\n";

                    reg << "extern \"C\" uint32_t fjell_api_version() { return FJELL_API_VERSION; }\n";
                    reg << "extern \"C\" void fjell_register(FjellTypeRegistry* registry) {\n";
                    reg << "    fjell_set_generated_table(fjreflect_generated, "
                        << manifest["types"].size() << ");\n";
                    reg << "    for (auto* r = fjell_auto_reg_head(); r; r = r->next) r->fn(registry);\n";
                    reg << "}\n";
                } else {
                    // Manifest parse failed — fallback
                    reg << "FJELL_REGISTER()\n";
                }
            } else {
                // No codegen — use simple registration
                reg << "FJELL_REGISTER()\n";
            }
        }
    }

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
            if (auto r = scripts.reload(scene, so_path); !r) {
                FJELL_CORE_ERROR("{}", r.error());
            }
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
    auto hpp_path = script_dir / (filename + ".hpp");
    auto cpp_path = script_dir / (filename + ".cpp");

    // Header — class definition with properties
    {
        std::ofstream file(hpp_path);
        if (file.is_open()) {
            file << "#pragma once\n"
                 << "#include \"fjell/registry.hpp\"\n"
                 << "#include \"fjell/world.hpp\"\n"
                 << "\n"
                 << "class " << class_name << " : public fjell::ScriptComponent {\n"
                 << "    FJELL_COMPONENT(" << class_name << ")\n"
                 << "\n"
                 << "    [[fj::property]] float speed = 0.0f;\n"
                 << "\n"
                 << "    void begin_play() override;\n"
                 << "    void tick(float dt) override;\n"
                 << "\n"
                 << "private:\n"
                 << "};\n";
        }
    }

    // Implementation
    {
        std::ofstream file(cpp_path);
        if (file.is_open()) {
            file << "#include \"" << filename << ".hpp\"\n"
                 << "\n"
                 << "void " << class_name << "::begin_play() {\n"
                 << "}\n"
                 << "\n"
                 << "void " << class_name << "::tick(float dt) {\n"
                 << "    (void)dt;\n"
                 << "}\n";
        }
    }

    FJELL_CORE_INFO("Created script: {}", hpp_path.string());
}

} // namespace fjell
