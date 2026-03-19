#include "core/engine.hpp"
#include "core/base_path.hpp"
#include "core/commands.hpp"
#include "core/game_builder.hpp"
#include "core/layer.hpp"
#include "core/log.hpp"
#include "physics/physics_world.hpp"
#include "ui/console.hpp"
#include "scene/camera_node.hpp"
#include "scene/light_node.hpp"
#include "renderer/bindless.hpp"
#include "renderer/fjsl_parser.hpp"
#include "renderer/material.hpp"
#include "renderer/material_asset.hpp"
#include "renderer/model.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/sprite_desc.hpp"
#include "renderer/texture.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>
#include <GLFW/glfw3.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

namespace fjell {

namespace {

class ViewportLayer : public Layer {
public:
    explicit ViewportLayer(bool* hovered)
        : Layer("Viewport"), hovered_{hovered} {}

    [[nodiscard]] bool wants_input() const override {
        return *hovered_ && !ImGui::GetIO().WantCaptureKeyboard;
    }

private:
    bool* hovered_;
};

} // namespace

Engine::Engine(std::string_view title, uint32_t width, uint32_t height,
               const std::filesystem::path& project_path)
    : window_{title, width, height}
    , vulkan_context_{window_}
#ifndef NDEBUG
    , shader_watcher_{base_path::resolve("../shaders"), base_path::resolve("shaders")}
#endif
{
    // Check for game mode (game.fjell next to binary)
    auto game_config_path = base_path::resolve("game.fjell");
    if (std::filesystem::exists(game_config_path)) {
        game_mode_ = true;
        std::ifstream gf(game_config_path);
        if (gf.is_open()) {
            auto gj = nlohmann::json::parse(gf, nullptr, false);
            if (!gj.is_discarded() && gj.contains("window")) {
                window_.set_title(gj["window"].value("title", "Fjell Game"));
            }
        }
    }

    // Open project if path given
    if (!project_path.empty()) {
        open_project(project_path);
    }

    window_.set_input(&input_);
    input_.attach(window_.handle());

    imgui_layer_ = std::make_unique<ImGuiLayer>(
        window_.handle(), vulkan_context_.instance(),
        vulkan_context_.physical_device(), vulkan_context_.device(),
        vulkan_context_.graphics_family(), vulkan_context_.graphics_queue(),
        vulkan_context_.swapchain_format(), vulkan_context_.image_count(),
        base_path::resolve("assets/fonts"));

    vulkan_context_.set_overlay_callback([this](VkCommandBuffer cmd) {
        imgui_layer_->render(cmd);
    });

    resource_manager_ = std::make_unique<ResourceManager>(
        vulkan_context_.device(), vulkan_context_.physical_device(),
        vulkan_context_.command_pool(), vulkan_context_.graphics_queue(),
        vulkan_context_.bindless_textures(), vulkan_context_.mega_buffer(),
        vulkan_context_.meshlet_buffer(), vulkan_context_.pipeline_cache());

    default_texture_ = resource_manager_->load_texture(
        base_path::resolve_asset("assets/textures/test.png", active_project()));

    scan_models();

    // Determine which scene to load
    if (project_) {
        current_scene_path_ = project_->resolve(project_->config().default_scene);
        scan_scenes();
    } else if (game_mode_) {
        // In game mode, scenes are alongside the binary
        current_scene_path_ = base_path::resolve("scenes/main.fjell");
    } else {
        current_scene_path_ = base_path::resolve(SCENE_FILE);
    }

    if (std::filesystem::exists(current_scene_path_)) {
        scene_ = Scene::load(current_scene_path_, *resource_manager_, active_project());
    } else {
        create_default_scene();
    }

    // Load game scripts
    if (game_mode_) {
        auto game_so = base_path::resolve("game.so");
        if (std::filesystem::exists(game_so)) {
            script_manager_.load(game_so);
            script_manager_.deserialize_all(scene_);
        }
    } else {
        load_project_scripts();
    }

    icon_cache_ = std::make_unique<IconCache>(
        vulkan_context_.device(), vulkan_context_.physical_device(),
        vulkan_context_.command_pool(), vulkan_context_.graphics_queue(),
        base_path::resolve("assets/icons"));

    content_browser_.init(icon_cache_.get(), &nvim_panel_,
                          &vulkan_context_, resource_manager_.get());
    content_browser_.set_add_model_callback(
        [this](const std::string& path) {
            add_model_to_scene(path);
            command_history_.execute(std::make_unique<AddObjectCommand>(
                scene_, selected_id_, &selected_id_));
        });
    content_browser_.set_load_scene_callback(
        [this](const std::string& path) { load_scene(path); });
    if (project_) {
        content_browser_.set_root(project_->root());
    }

    hierarchy_panel_.init(&scene_, &viewport_manager_, &selected_id_, &command_history_);
    hierarchy_panel_.set_add_primitive_callback(
        [this](const std::string& type) {
            add_primitive_to_scene(type);
            command_history_.execute(std::make_unique<AddObjectCommand>(
                scene_, selected_id_, &selected_id_));
        });
    hierarchy_panel_.set_remove_object_callback(
        [this](uint32_t id) {
            command_history_.execute(std::make_unique<RemoveObjectCommand>(
                scene_, id, &selected_id_,
                [this]() { vulkan_context_.wait_idle(); }));
        });

    properties_panel_.init(&scene_, &selected_id_, &gizmo_op_, &light_,
                           &script_manager_, &command_history_,
                           &vulkan_context_.grid_pass().settings(),
                           vulkan_context_.pipeline_cache(),
                           &vulkan_context_.bindless_textures());

    stats_panel_.init(&vulkan_context_);

    history_panel_.init(&command_history_);

    node_editor_panel_.init(&scene_, &selected_id_);

    layer_stack_.push_layer(std::make_unique<ViewportLayer>(&viewport_hovered_));

    viewport_manager_.init(vulkan_context_.device_ref(), vulkan_context_.msaa_samples());
    viewport_manager_.add_viewport(ViewportSource::final_color, "Viewport");

    FJELL_CORE_INFO("Engine initialized");
}

Engine::~Engine() {
    vulkan_context_.wait_idle();
    vulkan_context_.set_overlay_callback(nullptr);
    node_editor_panel_.shutdown();
    scene_ = {};
    resource_manager_.reset();
    icon_cache_.reset();
    viewport_manager_.release_textures();
    imgui_layer_.reset();
    FJELL_CORE_INFO("Engine shut down");
}

void Engine::scan_models() {
    available_models_.clear();
    const auto models_dir = std::filesystem::path(
        base_path::resolve_asset("assets/models", active_project()));
    if (!std::filesystem::exists(models_dir)) return;

    // Compute the root to make paths relative to (project root or engine root)
    auto root = project_ ? project_->root() : base_path::dir();

    for (const auto& entry : std::filesystem::directory_iterator(models_dir)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        if (ext == ".gltf" || ext == ".glb") {
            // Store relative path (e.g. "assets/models/cube.glb") for scene portability
            auto relative = std::filesystem::relative(entry.path(), root);
            available_models_.push_back(relative.string());
        }
    }
    std::sort(available_models_.begin(), available_models_.end());
    FJELL_CORE_INFO("Found {} model(s) in assets/models/", available_models_.size());
}

void Engine::save_material_assets() {
    auto* cache = vulkan_context_.pipeline_cache();
    auto& bindless = vulkan_context_.bindless_textures();
    if (!cache) return;

    for (auto& obj : scene_.objects()) {
        if (!obj->mesh || !obj->mesh->material) continue;
        if (!obj->mesh->material_name.ends_with(".fjmat")) continue;

        uint32_t pid = obj->mesh->material->pipeline_id();
        if (pid >= cache->shader_count()) continue;

        const auto& meta = cache->metadata(pid);
        uint32_t param_count = obj->mesh->material->custom_param_count();
        if (param_count == 0) continue;

        const auto& gpu = bindless.material_at(obj->mesh->material->material_id());

        // Read current params back from CPU buffer
        std::string mat_path = base_path::resolve_asset(
            obj->mesh->material_name, active_project());

        MaterialAsset asset;
        try {
            asset = load_material_asset(mat_path);
        } catch (...) {
            continue;
        }

        // Update custom param values from live buffer
        for (const auto& u : meta.uniforms) {
            if (u.type == UniformType::Sampler2D) continue;
            uint32_t fcount = uniform_float_count(u.type);
            std::vector<float> vals(fcount);
            for (uint32_t i = 0; i < fcount; ++i) {
                vals[i] = bindless.read_param(gpu.param_offset + u.slot + i);
            }
            asset.params[u.name] = vals;
        }

        // Save uv_scale from base params
        float uv_x = gpu.base_params[7];
        float uv_y = gpu.base_params[8];
        if (uv_x != 1.0f || uv_y != 1.0f) {
            asset.params["uv_scale"] = {uv_x, uv_y};
        } else {
            asset.params.erase("uv_scale");
        }

        save_material_asset(mat_path, asset);
    }
}

void Engine::create_default_scene() {
    vulkan_context_.wait_idle();
    auto tex = resource_manager_->load_texture(
        base_path::resolve_asset("assets/textures/test.png", active_project()));
    auto mat = resource_manager_->create_material("default", tex);

    auto plane = resource_manager_->create_primitive("plane");
    scene_.add(plane, mat, *resource_manager_,
               {{0.0f, 0.0f, 0.0f}, {}, {10.0f, 1.0f, 10.0f}},
               "Ground", "primitive:plane");

    auto cube = resource_manager_->create_primitive("cube");
    scene_.add(cube, mat, *resource_manager_,
               {{0.0f, 0.5f, 0.0f}},
               "Cube", "primitive:cube");

    auto sphere = resource_manager_->create_primitive("sphere");
    scene_.add(sphere, mat, *resource_manager_,
               {{2.5f, 0.5f, 0.0f}},
               "Sphere", "primitive:sphere");

    FJELL_CORE_INFO("Default scene created");
}

void Engine::add_model_to_scene(const std::string& path) {
    vulkan_context_.wait_idle();

    auto model_handle = resource_manager_->load_model(
        base_path::resolve_asset(path, active_project()));
    auto tex = resource_manager_->load_texture(
        base_path::resolve_asset("assets/textures/test.png", active_project()));
    auto mat_handle = resource_manager_->create_material("default", tex);
    std::string mat_name = "default";

    auto* model = resource_manager_->get(model_handle);
    if (model && !model->textures().empty()) {
        mat_name = path + "_mat";
        mat_handle = resource_manager_->create_material_from_texture(
            mat_name,
            const_cast<Texture*>(model->textures()[0].get()));
    }

    auto filename = std::filesystem::path(path).stem().string();
    selected_id_ = scene_.add(model_handle, mat_handle, *resource_manager_,
                              {}, filename, path, mat_name);
    FJELL_CORE_INFO("Added model to scene: {}", path);
}

void Engine::add_primitive_to_scene(const std::string& type) {
    vulkan_context_.wait_idle();

    // Empty node: no mesh
    if (type == "empty") {
        selected_id_ = scene_.add_empty();
        return;
    }

    // Camera node
    if (type == "camera") {
        auto node = std::make_unique<CameraNode>();
        node->name = "Camera";
        node->sprite = SpriteComponent{};
        node->sprite->texture_path = "assets/icons/icon_camera.png";
        node->sprite->editor_only = true;
        selected_id_ = scene_.add_node(std::move(node));
        return;
    }

    // Light nodes
    if (type == "light:directional") {
        auto node = std::make_unique<DirectionalLightNode>();
        node->name = "Directional Light";
        node->sprite = SpriteComponent{};
        node->sprite->texture_path = "assets/icons/icon_dir_light.png";
        node->sprite->editor_only = true;
        selected_id_ = scene_.add_node(std::move(node));
        return;
    }
    if (type == "light:point") {
        auto node = std::make_unique<PointLightNode>();
        node->name = "Point Light";
        node->sprite = SpriteComponent{};
        node->sprite->texture_path = "assets/icons/icon_point_light.png";
        node->sprite->editor_only = true;
        selected_id_ = scene_.add_node(std::move(node));
        return;
    }
    if (type == "light:spot") {
        auto node = std::make_unique<SpotLightNode>();
        node->name = "Spot Light";
        node->sprite = SpriteComponent{};
        node->sprite->texture_path = "assets/icons/icon_point_light.png";
        node->sprite->editor_only = true;
        selected_id_ = scene_.add_node(std::move(node));
        return;
    }

    // Sprite objects: plane primitive with a sprite component
    if (type == "sprite") {
        auto model_handle = resource_manager_->create_primitive("plane");
        if (!model_handle.valid()) return;

        auto tex = resource_manager_->load_texture(
            base_path::resolve_asset("assets/textures/test.png", active_project()));
        auto mat = resource_manager_->create_material("default", tex);

        selected_id_ = scene_.add(model_handle, mat, *resource_manager_,
                                  {}, "Sprite", "primitive:plane");
        auto* obj = scene_.find(selected_id_);
        if (obj) {
            obj->sprite = SpriteComponent{};
        }
        return;
    }

    auto model_handle = resource_manager_->create_primitive(type);
    if (!model_handle.valid()) return;

    auto tex = resource_manager_->load_texture(
        base_path::resolve_asset("assets/textures/test.png", active_project()));
    auto mat = resource_manager_->create_material("default", tex);

    std::string name = type;
    name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));

    selected_id_ = scene_.add(model_handle, mat, *resource_manager_,
                              {}, name, "primitive:" + type);
}

void Engine::setup_dockspace() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags host_flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoDocking;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});

    ImGui::Begin("##DockHost", nullptr, host_flags);
    ImGui::PopStyleVar(3);

    ImGuiID dockspace_id = ImGui::GetID("MainDockspace");
    ImGui::DockSpace(dockspace_id, {0.0f, 0.0f},
                     ImGuiDockNodeFlags_PassthruCentralNode);

    // Build default layout on first run (skip if restoring a saved layout)
    if (!layout_built_) {
        layout_built_ = true;
        focus_viewport_ = true;
        if (has_saved_layout_) {
            ImGui::End();
            return;
        }

        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->WorkSize);

        // Viewport is the parent — split everything else off from it
        ImGuiID viewport_node = dockspace_id;

        // Right panel: Hierarchy + Properties
        ImGuiID right = ImGui::DockBuilderSplitNode(viewport_node, ImGuiDir_Right, 0.22f, nullptr, &viewport_node);
        ImGuiID right_bottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.50f, nullptr, &right);

        // Stats/Console strip below viewport (tabbed)
        ImGuiID stats_strip = ImGui::DockBuilderSplitNode(viewport_node, ImGuiDir_Down, 0.20f, nullptr, &viewport_node);

        // Code Editor below stats
        ImGuiID editor = ImGui::DockBuilderSplitNode(stats_strip, ImGuiDir_Down, 0.65f, nullptr, &stats_strip);

        ImGui::DockBuilderDockWindow("Viewport", viewport_node);
        ImGui::DockBuilderDockWindow("Stats", stats_strip);
        ImGui::DockBuilderDockWindow("Console", stats_strip);
        ImGui::DockBuilderDockWindow("Code Editor", editor);
        ImGui::DockBuilderDockWindow("Hierarchy", right);
        ImGui::DockBuilderDockWindow("History", right);
        ImGui::DockBuilderDockWindow("Properties", right_bottom);
        ImGui::DockBuilderFinish(dockspace_id);
    }

    ImGui::End();
}

// Hierarchy and Properties panels extracted to ui/hierarchy_panel.cpp and ui/properties_panel.cpp

void Engine::draw_console() {
    auto& sink = log::console_sink();
    if (sink) {
        sink->draw("Console");
    }
}

void Engine::draw_code_editor() {
    nvim_panel_.poll();
    nvim_panel_.draw();

}

void Engine::open_project(const std::filesystem::path& path) {
    auto proj = Project::open(path);
    if (!proj) return;

    project_ = std::move(proj);
    window_.set_title("Fjell - " + project_->config().name);
    scan_models();
    scan_scenes();
    content_browser_.set_root(project_->root());

    // Defer layout restore to the first frame (ImGui needs a frame to set up docking)
    if (!project_->config().layout.empty()) {
        auto layout_path = project_->root() / "layouts" / (project_->config().layout + ".ini");
        if (std::filesystem::exists(layout_path)) {
            pending_layout_load_ = layout_path.string();
            active_layout_name_ = project_->config().layout;
            has_saved_layout_ = true;
        }
    }
}

void Engine::load_project_scripts() {
    if (!project_) return;
    auto so_path = project_->root() / "game.so";
    if (std::filesystem::exists(so_path)) {
        script_manager_.load(so_path);
        script_manager_.deserialize_all(scene_);
#ifndef NDEBUG
        script_manager_.watch(so_path);
#endif
    }
}

void Engine::close_project() {
    project_.reset();
    available_scenes_.clear();
    current_scene_path_ = base_path::resolve(SCENE_FILE);
    window_.set_title("Fjell Engine");
    scan_models();
    content_browser_.set_root({});
}

void Engine::scan_scenes() {
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

void Engine::load_scene(const std::string& path) {
    vulkan_context_.wait_idle();
    current_scene_path_ = path;
    scene_ = Scene::load(path, *resource_manager_, active_project());
    selected_id_ = 0;
    command_history_.clear();
}

void Engine::draw_menu_bar() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Save Scene")) {
                save_material_assets();
                scene_.save(current_scene_path_);
            }
            if (ImGui::MenuItem("Load Scene") && !current_scene_path_.empty()) {
                load_scene(current_scene_path_);
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Project")) {
            if (ImGui::MenuItem("New Project...")) {
                new_browser_.open("New Project");
            }
            if (ImGui::MenuItem("Open Project...")) {
                open_browser_.open("Open Project");
            }
            if (project_ && ImGui::MenuItem("Close Project")) {
                close_project();
            }
            ImGui::Separator();
            if (project_ && ImGui::MenuItem("Build Game")) {
                auto output = project_->root() / "build";
                GameBuilder::build(*project_, output);
            }
            ImGui::Separator();
            if (project_) {
                ImGui::TextDisabled("Project: %s", project_->config().name.c_str());
                ImGui::TextDisabled("Root: %s", project_->root().c_str());
            } else {
                ImGui::TextDisabled("No project open");
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Add Viewport")) {
                viewport_manager_.add_viewport();
            }
            ImGui::Separator();
            auto& grid = vulkan_context_.grid_pass().settings();
            ImGui::Checkbox("Show Grid", &grid.visible);
            ImGui::Checkbox("Snap to Grid", &grid.snap_enabled);
            ImGui::DragFloat("Grid Size", &grid.cell_size, 0.1f, 0.1f, 50.0f);
            ImGui::DragFloat("Grid Opacity", &grid.opacity, 0.01f, 0.0f, 1.0f);
            ImGui::DragFloat("Grid Fade", &grid.fade_distance, 1.0f, 10.0f, 500.0f);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Layout")) {
            // Save Layout — overwrite current active layout
            bool has_active = !active_layout_name_.empty();
            if (!has_active) ImGui::BeginDisabled();
            if (ImGui::MenuItem("Save Layout")) {
                auto layout_dir = project_ ? project_->root() / "layouts" : base_path::dir() / "layouts";
                std::filesystem::create_directories(layout_dir);
                auto path = layout_dir / (active_layout_name_ + ".ini");
                ImGui::SaveIniSettingsToDisk(path.string().c_str());
                if (project_) {
                    project_->config().layout = active_layout_name_;
                    project_->save();
                }
                FJELL_CORE_INFO("Layout saved: {}", path.string());
            }
            if (!has_active) ImGui::EndDisabled();

            // Save Layout As — always available, prompts for name
            if (ImGui::MenuItem("Save Layout As...")) {
                show_save_layout_popup_ = true;
                std::memset(layout_name_buf_, 0, sizeof(layout_name_buf_));
            }
            ImGui::Separator();

            // List saved layouts
            auto layout_dir = project_ ? project_->root() / "layouts" : base_path::dir() / "layouts";
            if (std::filesystem::is_directory(layout_dir)) {
                for (const auto& entry : std::filesystem::directory_iterator(layout_dir)) {
                    if (entry.path().extension() != ".ini") continue;
                    auto name = entry.path().stem().string();
                    bool is_active = (name == active_layout_name_);
                    if (ImGui::MenuItem(name.c_str(), nullptr, is_active)) {
                        ImGui::LoadIniSettingsFromDisk(entry.path().string().c_str());
                        active_layout_name_ = name;
                        if (project_) {
                            project_->config().layout = name;
                            project_->save();
                        }
                    }
                }
            }
            ImGui::EndMenu();
        }

        if (project_ && ImGui::BeginMenu("Scripts")) {
            if (ImGui::MenuItem("New Script...")) {
                show_new_script_popup_ = true;
                std::memset(new_script_name_, 0, sizeof(new_script_name_));
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Build & Reload")) {
                // Refresh scripting headers in project so scripts see latest API
                Project::refresh_headers(*project_);

                auto scripts_dir = project_->root() / "scripts";
                auto build_dir = scripts_dir / "build";

                // Auto-generate register_all.cpp by scanning for FJELL_COMPONENT macros
                {
                    std::vector<std::pair<std::string, std::string>> components; // {filename, classname}
                    for (const auto& entry : std::filesystem::directory_iterator(scripts_dir)) {
                        if (!entry.is_regular_file()) continue;
                        auto ext = entry.path().extension().string();
                        if (ext != ".cpp" && ext != ".hpp") continue;
                        if (entry.path().filename() == "register_all.cpp") continue;

                        std::ifstream src(entry.path());
                        std::string line;
                        while (std::getline(src, line)) {
                            auto pos = line.find("FJELL_COMPONENT(");
                            if (pos == std::string::npos) continue;
                            auto start = pos + 16; // length of "FJELL_COMPONENT("
                            auto end = line.find(')', start);
                            if (end == std::string::npos) continue;
                            std::string cls = line.substr(start, end - start);
                            components.emplace_back(entry.path().filename().string(), cls);
                        }
                    }

                    // Generate register_all.cpp with the single FJELL_REGISTER() entry point.
                    // Each component auto-registers via static init from FJELL_COMPONENT().
                    std::ofstream reg(scripts_dir / "register_all.cpp");
                    if (reg.is_open()) {
                        reg << "// Auto-generated — do not edit.\n";
                        reg << "#include \"scripting/fjell_registry.hpp\"\n\n";
                        reg << "FJELL_REGISTER()\n";
                    }
                }

                // Reconfigure to pick up new/removed .cpp files, then build
                auto cmd = "cmake -B " + build_dir.string() + " -S " + scripts_dir.string() +
                           " -DCMAKE_BUILD_TYPE=Release 2>&1 && "
                           "cmake --build " + build_dir.string() + " -j$(nproc) 2>&1";

                FJELL_CORE_INFO("Building scripts...");

                FILE* pipe = popen(cmd.c_str(), "r");
                bool success = false;
                if (pipe) {
                    std::array<char, 512> line{};
                    while (fgets(line.data(), static_cast<int>(line.size()), pipe)) {
                        std::string s{line.data()};
                        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
                            s.pop_back();
                        }
                        if (!s.empty()) {
                            if (s.find("error") != std::string::npos) {
                                FJELL_CORE_ERROR("[build] {}", s);
                            } else if (s.find("warning") != std::string::npos) {
                                FJELL_CORE_WARN("[build] {}", s);
                            } else {
                                FJELL_CORE_INFO("[build] {}", s);
                            }
                        }
                    }
                    int ret = pclose(pipe);
                    success = (ret == 0);
                }

                if (success) {
                    FJELL_CORE_INFO("Scripts built successfully");
                    auto so_path = project_->root() / "game.so";
                    if (std::filesystem::exists(so_path)) {
                        script_manager_.reload(scene_, so_path);
                    }
                } else {
                    FJELL_CORE_ERROR("Script build failed");
                }
            }
            ImGui::EndMenu();
        }

        // Play/Stop button in menu bar
        float bar_w = ImGui::GetWindowWidth();
        float button_w = 60.0f;
        ImGui::SameLine(bar_w * 0.5f - button_w * 0.5f);
        if (is_playing()) {
            if (ImGui::SmallButton("Stop")) {
                stop_play();
            }
        } else {
            if (ImGui::SmallButton("Play")) {
                start_play();
            }
        }

        ImGui::EndMainMenuBar();
    }

    // Save Layout As popup
    if (show_save_layout_popup_) {
        ImGui::OpenPopup("Save Layout As");
        show_save_layout_popup_ = false;
    }
    if (ImGui::BeginPopupModal("Save Layout As", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Layout Name:");
        ImGui::SetNextItemWidth(250.0f);
        bool enter = ImGui::InputText("##LayoutName", layout_name_buf_, sizeof(layout_name_buf_),
                                       ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);

        bool valid = std::strlen(layout_name_buf_) > 0;
        if (!valid) ImGui::BeginDisabled();
        bool save = ImGui::Button("Save", {120, 0}) || (enter && valid);
        if (!valid) ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {120, 0})) ImGui::CloseCurrentPopup();

        if (save) {
            auto layout_dir = project_ ? project_->root() / "layouts" : base_path::dir() / "layouts";
            std::filesystem::create_directories(layout_dir);
            auto path = layout_dir / (std::string(layout_name_buf_) + ".ini");
            ImGui::SaveIniSettingsToDisk(path.string().c_str());
            active_layout_name_ = layout_name_buf_;
            if (project_) {
                project_->config().layout = active_layout_name_;
                project_->save();
            }
            FJELL_CORE_INFO("Layout saved: {}", path.string());
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // New Script popup
    if (show_new_script_popup_) {
        ImGui::OpenPopup("New Script");
        show_new_script_popup_ = false;
    }
    if (ImGui::BeginPopupModal("New Script", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Class Name:");
        ImGui::SetNextItemWidth(300.0f);
        bool enter = ImGui::InputText("##ClassName", new_script_name_, sizeof(new_script_name_),
                                       ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);

        bool valid = std::strlen(new_script_name_) > 0;
        if (!valid) ImGui::BeginDisabled();
        bool create = ImGui::Button("Create", {140, 0}) || (enter && valid);
        if (!valid) ImGui::EndDisabled();
        ImGui::SameLine();
        bool cancel = ImGui::Button("Cancel", {140, 0});

        if (create && project_) {
            std::string name = new_script_name_;
            // Lowercase filename
            std::string filename = name;
            for (auto& c : filename) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            auto script_dir = project_->root() / "scripts";
            auto path = script_dir / (filename + ".cpp");

            std::ofstream file(path);
            if (file.is_open()) {
                file << "#include \"scripting/fjell_registry.hpp\"\n"
                     << "#include \"scripting/fjell_world.hpp\"\n"
                     << "#include <array>\n"
                     << "#include <cmath>\n"
                     << "\n"
                     << "class " << name << " : public fjell::ScriptComponent {\n"
                     << "  FJELL_COMPONENT(" << name << ")\n"
                     << "\n"
                     << "  // Fields here show up in Properties. Types: FJELL_FIELD_FLOAT, _VEC3, _INT\n"
                     << "  // FJELL_F_RANGE(Class, member, type, min, max, drag_speed) for sliders\n"
                     << "  static auto reflect() {\n"
                     << "    return std::array{\n"
                     << "      // FJELL_F(" << name << ", speed, FJELL_FIELD_FLOAT),\n"
                     << "    };\n"
                     << "  }\n"
                     << "\n"
                     << "  void begin_play() override {\n"
                     << "  }\n"
                     << "\n"
                     << "  // actor()->transform().position / .rotation / .scale\n"
                     << "  // world()->log_info(\"msg\")\n"
                     << "  void tick(float dt) override {\n"
                     << "    (void)dt;\n"
                     << "  }\n"
                     << "\n"
                     << "private:\n"
                     << "  // float speed = 1.0f;\n"
                     << "};\n";

                FJELL_CORE_INFO("Created script: {}", path.string());
            } else {
                FJELL_CORE_ERROR("Failed to create script: {}", path.string());
            }
            ImGui::CloseCurrentPopup();
        }
        if (cancel) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // File browser popups
    if (open_browser_.draw()) {
        open_project(open_browser_.selected_path());
        if (project_) {
            current_scene_path_ = project_->resolve(project_->config().default_scene);
            if (std::filesystem::exists(current_scene_path_)) {
                load_scene(current_scene_path_);
            } else {
                scene_.clear();
                create_default_scene();
            }
            load_project_scripts();
        }
    }

    if (new_browser_.draw()) {
        project_ = Project::create(new_browser_.selected_path(),
                                   new_browser_.project_name());
        if (project_) {
            window_.set_title("Fjell - " + project_->config().name);
            scan_models();
            scan_scenes();
            content_browser_.set_root(project_->root());
            current_scene_path_ = project_->resolve(project_->config().default_scene);
            scene_.clear();
            create_default_scene();
        }
    }
}

void Engine::draw_ui() {
    // Update window title with unsaved indicator
    {
        std::string title = "Fjell";
        if (project_) title += " - " + project_->config().name;
        if (scene_.is_dirty()) title += " *";
        window_.set_title(title);
    }

    // Global shortcuts
    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S)) {
        save_material_assets();
        scene_.save(current_scene_path_);
    }
    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Space)) {
        content_browser_.toggle_visibility();
    }
    if (!ImGui::GetIO().WantTextInput && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z)) {
        if (ImGui::GetIO().KeyShift) {
            command_history_.redo();
        } else {
            command_history_.undo();
        }
    }
    if (!ImGui::GetIO().WantTextInput && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) {
        command_history_.redo();
    }

    // Gizmo keyboard shortcuts (skip when typing in a text field or navigating)
    bool navigating = input_.is_mouse_button_held(GLFW_MOUSE_BUTTON_RIGHT);
    if (!ImGui::GetIO().WantTextInput && !navigating) {
        if (ImGui::IsKeyPressed(ImGuiKey_W)) gizmo_op_ = 0; // translate
        if (ImGui::IsKeyPressed(ImGuiKey_E)) gizmo_op_ = 1; // rotate
        if (ImGui::IsKeyPressed(ImGuiKey_R)) gizmo_op_ = 2; // scale
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) selected_id_ = 0;
    }

    // Apply deferred layout load
    if (!pending_layout_load_.empty()) {
        ImGui::LoadIniSettingsFromDisk(pending_layout_load_.c_str());
        pending_layout_load_.clear();
    }

    draw_menu_bar();
    setup_dockspace();
    viewport_manager_.draw(vulkan_context_, scene_, selected_id_, input_, clock_.delta_time(), gizmo_op_);
    viewport_hovered_ = viewport_manager_.any_hovered();
    hierarchy_panel_.draw();
    properties_panel_.draw();
    content_browser_.draw();
    stats_panel_.draw(clock_.delta_time());
    history_panel_.draw();
    node_editor_panel_.draw();
    draw_console();
    draw_code_editor();

    if (focus_viewport_) {
        ImGui::SetWindowFocus("Viewport");
        focus_viewport_ = false;
    }
}

void Engine::start_play() {
    command_history_.clear();
    script_manager_.serialize_all(scene_);
    play_session_ = std::make_unique<PlaySession>(scene_);
    script_manager_.prepare(scene_, input_, clock_, play_session_->physics_world());
    script_manager_.deserialize_all(scene_);
    script_manager_.call_begin_play(scene_);
    vulkan_context_.set_playing(true);
    input_.set_cursor_captured(window_.handle(), true);
    selected_id_ = 0;
    FJELL_CORE_INFO("Play mode started");
}

void Engine::stop_play() {
    if (game_mode_) {
        // In game mode, ESC quits the application
        glfwSetWindowShouldClose(window_.handle(), GLFW_TRUE);
        return;
    }
    script_manager_.call_end_play(scene_);
    play_session_->restore(scene_);
    script_manager_.deserialize_all(scene_);
    vulkan_context_.set_playing(false);
    input_.set_cursor_captured(window_.handle(), false);
    play_session_.reset();
    focus_viewport_ = true;
    FJELL_CORE_INFO("Play mode stopped");
}

void Engine::draw_play_overlay() {
    // In play mode, resize the active viewport's offscreen to fill the window
    auto w = window_.width();
    auto h = window_.height();
    viewport_manager_.resize_active(w, h);

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoScrollbar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::Begin("##PlayView", nullptr, flags);
    ImGui::PopStyleVar();

    // Scene image fills the window — use active viewport's offscreen
    auto tex_id = viewport_manager_.active_texture(vulkan_context_);
    if (tex_id) {
        ImGui::Image(tex_id, viewport->WorkSize);
    }

    // Overlay bar at top center
    float bar_width = 220.0f;
    float bar_height = 32.0f;
    float bar_x = viewport->WorkPos.x + (viewport->WorkSize.x - bar_width) * 0.5f;
    float bar_y = viewport->WorkPos.y + 8.0f;

    ImGui::SetNextWindowPos({bar_x, bar_y});
    ImGui::SetNextWindowSize({bar_width, bar_height});
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("##PlayBar", nullptr,
                 ImGuiWindowFlags_NoTitleBar |
                 ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoDocking |
                 ImGuiWindowFlags_NoFocusOnAppearing);

    ImGui::Text("Press ESC to stop");
    ImGui::SameLine();
    if (ImGui::SmallButton("Stop")) {
        stop_play();
    }
    ImGui::End();

    ImGui::End(); // ##PlayView
}

void Engine::run() {
    FJELL_CORE_INFO("Entering main loop");

    // Game mode: start playing immediately
    if (game_mode_) {
        start_play();
    }

    while (!window_.should_close()) {
        input_.update();
        window_.poll_events();
        clock_.tick();

#ifndef NDEBUG
        if (shader_watcher_.poll()) {
            vulkan_context_.reload_pipeline();
        }
#endif


        float dt = clock_.delta_time();

        if (is_playing()) {
            script_manager_.update(scene_, input_, clock_, play_session_->physics_world());
            play_session_->physics_step(dt, scene_);
            if (play_session_->wants_stop(input_)) {
                stop_play();
            }
        }

#ifndef NDEBUG
        (void)script_manager_.poll_reload(scene_);
#endif

        scene_.compute_world_matrices();

        // Resize offscreen passes before ImGui frame so descriptors stay valid
        viewport_manager_.apply_resize(vulkan_context_);

        imgui_layer_->begin_frame();
        if (is_playing()) {
            draw_play_overlay();
        } else {
            draw_ui();
        }
        imgui_layer_->end_frame();

        auto requests = viewport_manager_.build_render_requests();

        // In play mode, override the active viewport's camera with the scene CameraNode
        if (is_playing() && !requests.empty()) {
            for (const auto& obj : scene_.objects()) {
                if (auto* cam_node = dynamic_cast<const CameraNode*>(obj.get())) {
                    float aspect = viewport_manager_.active_aspect();
                    requests[0].view = cam_node->view_matrix();
                    requests[0].projection = cam_node->projection_matrix(aspect);
                    requests[0].camera_position = cam_node->position();
                    break;
                }
            }
        }

        vulkan_context_.sync_sprite_textures(scene_, *resource_manager_);
        vulkan_context_.draw_frame(requests, light_, &scene_, clock_.delta_time(), selected_id_, thread_pool_);
    }
    FJELL_CORE_INFO("Exiting main loop");
}

} // namespace fjell
