#include "core/project_manager.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"
#include "platform/platform.hpp"
#include "scene/scene.hpp"
#include "scripting/script_manager.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fjell {
namespace {

/// Locate the fjreflect codegen tool. Covers the dev build tree, MSVC's
/// per-config subdirectories, and the install layout.
std::filesystem::path find_fjreflect() {
    namespace fs = std::filesystem;
    const auto exe = std::string("fjreflect") + platform::EXE_EXT;
    const auto& root = base_path::dir();

    const std::array candidates{
        root / "tools" / "fjreflect" / exe,
        root / "tools" / "fjreflect" / "Debug" / exe,
        root / "tools" / "fjreflect" / "Release" / exe,
        root / "bin" / exe,
        root / exe,
    };

    for (const auto& path : candidates) {
        if (fs::exists(path)) return path;
    }

    std::string searched;
    for (const auto& path : candidates) {
        searched += "\n  " + path.string();
    }
    FJELL_CORE_ERROR(
        "fjreflect not found — component reflection will be skipped, so [[fj::property]] "
        "fields and FJELL_COMPONENT types will not appear in the editor. Searched:{}",
        searched);
    return {};
}

} // namespace

void ProjectManager::open(const std::filesystem::path& path) {
    auto proj = Project::open(path);
    if (!proj) return;
    project_ = std::move(proj);
    scan_models();
    scan_scenes();

#ifndef NDEBUG
    setup_script_watcher();
#endif

    on_opened.broadcast(project_->config().name);
}

Project ProjectManager::create(const std::filesystem::path& directory,
                                const std::string& name) {
    auto proj = Project::create(directory, name);
    project_ = std::move(proj);
    scan_models();
    scan_scenes();

#ifndef NDEBUG
    setup_script_watcher();
#endif

    on_opened.broadcast(project_->config().name);
    return proj;
}

void ProjectManager::close() {
#ifndef NDEBUG
    script_watcher_.clear();
    source_changed_ = false;
#endif

    // Block until any in-flight build finishes before tearing down project
    if (build_future_.valid()) {
        build_future_.wait();
    }

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
        if (ext == ".fjmesh") {
            auto relative = std::filesystem::relative(entry.path(), root);
            available_models_.push_back(relative.string());
        }
    }
}

// ── Async build ──────────────────────────────────────────────────────────

void ProjectManager::build_scripts(ScriptManager& /*scripts*/, Scene& /*scene*/) {
    if (!project_) return;
    if (build_future_.valid()) {
        FJELL_CORE_WARN("Script build already in progress");
        return;
    }

    Project::refresh_headers(*project_);

    // Snapshot paths for the background thread (no Project access after this)
    namespace fs = std::filesystem;
    auto project_root = project_->root();
    auto scripts_dir = project_root / "scripts";
    auto build_dir = scripts_dir / "build";
    auto generated_dir = scripts_dir / "generated";
    auto so_path = project_root / (std::string("game") + platform::SHARED_LIB_EXT);

    auto fjreflect_path = find_fjreflect();
    auto sdk_include = base_path::sdk_include();

    FJELL_CORE_INFO("Building scripts...");

    build_future_ = std::async(std::launch::async,
        [scripts_dir, build_dir, generated_dir, so_path,
         fjreflect_path, sdk_include, project_root]() -> BuildResult {
            // Write CMakeLists.txt on the background thread to avoid
            // stack overflow on the main thread (deep ImGui call stack).
            Project::write_scripts_cmake_to(
                project_root / "scripts" / "CMakeLists.txt",
                sdk_include);

            fs::create_directories(generated_dir);

            std::string all_output;

            // ── fjreflect codegen ────────────────────────────────────
            bool has_codegen = false;
            if (fs::exists(fjreflect_path)) {
                std::string source_list;
                for (const auto& entry : fs::directory_iterator(scripts_dir)) {
                    if (!entry.is_regular_file()) continue;
                    auto name = entry.path().filename().string();
                    if (!name.ends_with(".hpp")) continue;
                    source_list += " " + entry.path().string();
                }

                if (!source_list.empty()) {
                    auto glm_dir = build_dir / "_deps" / "glm-src";

#ifdef _WIN32
    #ifdef FJELL_CXX_COMPILER
                    auto reflect_cmd = fjreflect_path.string() +
                        " --output-dir " + generated_dir.string() +
                        " --include-dir " + sdk_include.string() +
                        " --compiler \"" FJELL_CXX_COMPILER "\"";
    #else
                    auto reflect_cmd = fjreflect_path.string() +
                        " --output-dir " + generated_dir.string() +
                        " --include-dir " + sdk_include.string() +
                        " --compiler cl";
    #endif
#else
                    auto reflect_cmd = fjreflect_path.string() +
                        " --output-dir " + generated_dir.string() +
                        " --include-dir " + sdk_include.string() +
                        " --compiler g++";
#endif

                    if (fs::exists(glm_dir)) {
                        reflect_cmd += " --include-dir " + glm_dir.string();
                    }

                    reflect_cmd += source_list + " 2>&1";

                    std::string reflect_output;
                    int reflect_ret = platform::run_command(reflect_cmd, reflect_output);
                    if (reflect_ret != 0) {
                        return {.success = false,
                                .output = "[fjreflect] codegen failed:\n" + reflect_output,
                                .so_path = so_path};
                    }
                    all_output += reflect_output;
                    has_codegen = true;
                }
            }

            // ── Generate register_all.cpp ────────────────────────────
            {
                std::ofstream reg(scripts_dir / "register_all.cpp");
                if (reg.is_open()) {
                    reg << "// Auto-generated -- do not edit.\n";
                    reg << "#include \"fjell/registry.hpp\"\n\n";

                    auto manifest_path = generated_dir / "manifest.json";
                    if (has_codegen && fs::exists(manifest_path)) {
                        std::ifstream mf(manifest_path);
                        auto manifest = nlohmann::json::parse(mf, nullptr, false);
                        if (manifest.is_object() && manifest.contains("types")) {
                            std::vector<nlohmann::json> script_types;
                            std::vector<nlohmann::json> ui_types;
                            for (const auto& t : manifest["types"]) {
                                bool is_ui = t.value("is_ui_component", false);
                                if (is_ui) {
                                    ui_types.push_back(t);
                                } else {
                                    script_types.push_back(t);
                                }
                            }

                            for (const auto& t : manifest["types"]) {
                                auto name = t["name"].get<std::string>();
                                auto fields_sym = t["fields_symbol"].get<std::string>();
                                reg << "extern \"C\" const FjellFieldDesc " << fields_sym << "[];\n";
                                reg << "extern \"C\" const uint32_t fjreflect_" << name << "_field_count;\n";
                                if (t.contains("trace_symbol") && !t["trace_symbol"].is_null()) {
                                    reg << "extern \"C\" void " << t["trace_symbol"].get<std::string>()
                                        << "(void*, void*);\n";
                                }
                                if (t.value("rpc_count", 0u) > 0) {
                                    reg << "extern \"C\" const FjellRpcDesc fjreflect_" << name << "_rpcs[];\n";
                                }
                            }

                            if (!script_types.empty()) {
                                reg << "\nstatic const FjellGeneratedInfo fjreflect_generated[] = {\n";
                                for (const auto& t : script_types) {
                                    auto name = t["name"].get<std::string>();
                                    auto fields_sym = t["fields_symbol"].get<std::string>();
                                    auto count = t["field_count"].get<uint32_t>();
                                    auto rpc_count = t.value("rpc_count", 0u);
                                    reg << "    {\"" << name << "\", " << fields_sym
                                        << ", " << count << ", ";
                                    if (t.contains("trace_symbol") && !t["trace_symbol"].is_null()) {
                                        reg << t["trace_symbol"].get<std::string>();
                                    } else {
                                        reg << "nullptr";
                                    }
                                    if (rpc_count > 0) {
                                        reg << ", fjreflect_" << name << "_rpcs, " << rpc_count;
                                    } else {
                                        reg << ", nullptr, 0";
                                    }
                                    reg << "},\n";
                                }
                                reg << "};\n";
                            }

                            if (!ui_types.empty()) {
                                reg << "\n#include \"fjell/ui_registry.hpp\"\n";
                                reg << "static const FjellUiGeneratedInfo fjreflect_ui_generated[] = {\n";
                                for (const auto& t : ui_types) {
                                    auto name = t["name"].get<std::string>();
                                    auto fields_sym = t["fields_symbol"].get<std::string>();
                                    auto count = t["field_count"].get<uint32_t>();
                                    auto action_count = t.value("action_count", 0u);
                                    reg << "    {\"" << name << "\", "
                                        << fields_sym << ", " << count << ", ";
                                    if (t.contains("trace_symbol") && !t["trace_symbol"].is_null()) {
                                        reg << t["trace_symbol"].get<std::string>();
                                    } else {
                                        reg << "nullptr";
                                    }
                                    reg << ", ";
                                    if (action_count > 0) {
                                        reg << "fjreflect_" << name << "_ui_actions, " << action_count;
                                    } else {
                                        reg << "nullptr, 0";
                                    }
                                    reg << "},\n";
                                }
                                reg << "};\n";
                            }

                            reg << "\nextern \"C\" FJELL_GAME_EXPORT uint32_t fjell_api_version() { return FJELL_API_VERSION; }\n";
                            reg << "extern \"C\" FJELL_GAME_EXPORT void fjell_register(FjellTypeRegistry* registry) {\n";
                            if (!script_types.empty()) {
                                reg << "    fjell_set_generated_table(fjreflect_generated, "
                                    << script_types.size() << ");\n";
                            }
                            if (!ui_types.empty()) {
                                reg << "    fjell_set_ui_generated_table(fjreflect_ui_generated, "
                                    << ui_types.size() << ");\n";
                            }
                            reg << "    for (auto* r = fjell_auto_reg_head(); r; r = r->next) r->fn(registry);\n";
                            reg << "    for (auto* r = fjell_ui_auto_reg_head(); r; r = r->next) r->fn(registry);\n";
                            reg << "}\n";
                        } else {
                            reg << "FJELL_REGISTER()\n";
                        }
                    } else {
                        reg << "FJELL_REGISTER()\n";
                    }
                }
            }

            // ── cmake configure + build ──────────────────────────────
            auto cmd = "cmake -B " + build_dir.string() + " -S " + scripts_dir.string() +
                       " -DCMAKE_BUILD_TYPE=Release 2>&1 && "
                       "cmake --build " + build_dir.string() + " --parallel 2>&1";

            std::string build_output;
            int ret = platform::run_command(cmd, build_output);
            all_output += build_output;
            return {.success = (ret == 0), .output = std::move(all_output), .so_path = so_path};
        });
}

void ProjectManager::poll_build(ScriptManager& scripts, Scene& scene, bool playing) {
#ifndef NDEBUG
    // Check for source file changes — auto-trigger build after a debounce
    (void)script_watcher_.poll();
    if (source_changed_ && !build_future_.valid()) {
        auto elapsed = std::chrono::steady_clock::now() - last_source_change_;
        if (elapsed >= std::chrono::milliseconds(500)) {
            source_changed_ = false;
            FJELL_CORE_INFO("Source files changed — auto-building scripts");
            build_scripts(scripts, scene);
        }
    }
#endif

    if (!build_future_.valid()) return;
    if (build_future_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;

    auto result = build_future_.get(); // resets the future to invalid — is_building() returns false

    // Log build output on main thread
    std::istringstream iss(result.output);
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

    if (result.success) {
        FJELL_CORE_INFO("Scripts built successfully");
        if (std::filesystem::exists(result.so_path)) {
            if (auto r = scripts.reload(scene, result.so_path); !r) {
                FJELL_CORE_ERROR("{}", r.error());
            } else if (playing) {
                scripts.call_begin_play(scene);
            }
        }
    } else {
        FJELL_CORE_ERROR("Script build failed");
    }
}

// ── Source file watcher ──────────────────────────────────────────────────

#ifndef NDEBUG
void ProjectManager::setup_script_watcher() {
    script_watcher_.clear();
    source_changed_ = false;
    if (!project_) return;

    auto scripts_dir = project_->root() / "scripts";
    if (!std::filesystem::exists(scripts_dir)) return;

    script_watcher_.watch(scripts_dir, [this](const std::filesystem::path& path) {
        auto ext = path.extension().string();
        if (ext == ".cpp" || ext == ".hpp" || ext == ".h") {
            // Ignore generated files
            auto filename = path.filename().string();
            if (filename == "register_all.cpp") return;
            if (path.string().find("/generated/") != std::string::npos) return;

            source_changed_ = true;
            last_source_change_ = std::chrono::steady_clock::now();
        }
    });

    FJELL_CORE_INFO("Watching scripts/ for changes: {}", scripts_dir.string());
}
#endif

// ── Script creation ──────────────────────────────────────────────────────

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
