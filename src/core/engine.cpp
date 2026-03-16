#include "core/engine.hpp"
#include "core/base_path.hpp"
#include "core/game_builder.hpp"
#include "core/log.hpp"
#include "renderer/material.hpp"
#include "renderer/model.hpp"
#include "renderer/render_stats.hpp"
#include "renderer/texture.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>
#include <GLFW/glfw3.h>
#include <glm/gtc/type_ptr.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

namespace fjell {

Engine::Engine(std::string_view title, uint32_t width, uint32_t height,
               const std::filesystem::path& project_path)
    : window_{title, width, height}
    , camera_{{0.0f, 1.0f, 0.0f}, 8.0f}
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
        vulkan_context_.render_pass(), vulkan_context_.image_count());

    vulkan_context_.set_overlay_callback([this](VkCommandBuffer cmd) {
        imgui_layer_->render(cmd);
    });

    resource_manager_ = std::make_unique<ResourceManager>(
        vulkan_context_.device(), vulkan_context_.physical_device(),
        vulkan_context_.command_pool(), vulkan_context_.graphics_queue(),
        vulkan_context_.material_pool(), vulkan_context_.material_layout());

    resource_manager_->load_texture(
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

    FJELL_CORE_INFO("Engine initialized");
}

Engine::~Engine() {
    vulkan_context_.wait_idle();
    vulkan_context_.set_overlay_callback(nullptr);
    scene_ = {};
    resource_manager_.reset();
    vulkan_context_.release_viewport_texture();
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

void Engine::create_default_scene() {
    vulkan_context_.wait_idle();
    auto* tex = resource_manager_->load_texture(
        base_path::resolve_asset("assets/textures/test.png", active_project()));
    auto* mat = resource_manager_->create_material("default", tex);

    auto* plane = resource_manager_->create_primitive("plane");
    scene_.add(plane, mat, {{0.0f, 0.0f, 0.0f}, {}, {10.0f, 1.0f, 10.0f}},
               "Ground", "primitive:plane");

    auto* cube = resource_manager_->create_primitive("cube");
    scene_.add(cube, mat, {{0.0f, 0.5f, 0.0f}},
               "Cube", "primitive:cube");

    auto* sphere = resource_manager_->create_primitive("sphere");
    scene_.add(sphere, mat, {{2.5f, 0.5f, 0.0f}},
               "Sphere", "primitive:sphere");

    FJELL_CORE_INFO("Default scene created");
}

void Engine::add_model_to_scene(const std::string& path) {
    vulkan_context_.wait_idle();

    auto* model = resource_manager_->load_model(
        base_path::resolve_asset(path, active_project()));
    auto* tex = resource_manager_->load_texture(
        base_path::resolve_asset("assets/textures/test.png", active_project()));
    auto* mat = resource_manager_->create_material("default", tex);
    std::string mat_name = "default";

    if (!model->textures().empty()) {
        mat_name = path + "_mat";
        mat = resource_manager_->create_material(
            mat_name,
            const_cast<Texture*>(model->textures()[0].get()));
    }

    auto filename = std::filesystem::path(path).stem().string();
    selected_id_ = scene_.add(model, mat, {}, filename, path, mat_name);
    FJELL_CORE_INFO("Added model to scene: {}", path);
}

void Engine::add_primitive_to_scene(const std::string& type) {
    vulkan_context_.wait_idle();

    auto* model = resource_manager_->create_primitive(type);
    if (!model) return;

    auto* tex = resource_manager_->load_texture(
        base_path::resolve_asset("assets/textures/test.png", active_project()));
    auto* mat = resource_manager_->create_material("default", tex);

    std::string name = type;
    name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));

    selected_id_ = scene_.add(model, mat, {}, name, "primitive:" + type);
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

    // Build default layout on first run
    if (!layout_built_) {
        layout_built_ = true;

        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->WorkSize);

        ImGuiID center = dockspace_id;
        ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.18f, nullptr, &center);
        ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.22f, nullptr, &center);
        ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.18f, nullptr, &center);
        ImGuiID right_bottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.50f, nullptr, &right);

        ImGui::DockBuilderDockWindow("Viewport", center);
        ImGui::DockBuilderDockWindow("Assets", left);
        ImGui::DockBuilderDockWindow("Hierarchy", right);
        ImGui::DockBuilderDockWindow("Properties", right_bottom);
        ImGui::DockBuilderDockWindow("Stats", bottom);

        ImGui::DockBuilderFinish(dockspace_id);
    }

    ImGui::End();
}

void Engine::draw_viewport() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.0f, 0.0f, 0.0f, 0.0f});

    if (ImGui::Begin("Viewport")) {
        // Play button toolbar
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {4.0f, 4.0f});
        if (ImGui::Button(is_playing() ? "Stop" : "Play")) {
            if (is_playing()) stop_play();
            else start_play();
        }
        ImGui::PopStyleVar();

        auto vp_min = ImGui::GetWindowContentRegionMin();
        auto vp_max = ImGui::GetWindowContentRegionMax();
        auto vp_pos = ImGui::GetWindowPos();
        viewport_pos_ = {vp_pos.x + vp_min.x, vp_pos.y + vp_min.y};
        viewport_size_ = {vp_max.x - vp_min.x, vp_max.y - vp_min.y};

        // Resize offscreen pass to match viewport
        auto vp_w = static_cast<uint32_t>(viewport_size_.x);
        auto vp_h = static_cast<uint32_t>(viewport_size_.y);
        if (vp_w > 0 && vp_h > 0) {
            vulkan_context_.resize_viewport(vp_w, vp_h);
        }

        // Display rendered scene as texture
        auto tex_id = vulkan_context_.viewport_texture();
        if (tex_id) {
            ImGui::Image(tex_id, {viewport_size_.x, viewport_size_.y});
        }

        // Gizmo for selected object (draw over the image)
        if (auto* obj = scene_.find(selected_id_); obj) {
            float aspect = viewport_size_.x / std::max(viewport_size_.y, 1.0f);
            auto view = camera_.view_matrix();
            auto projection = glm::perspective(
                glm::radians(camera_.fov()), aspect, 0.1f, 1000.0f);

            ImGuizmo::SetRect(viewport_pos_.x, viewport_pos_.y,
                              viewport_size_.x, viewport_size_.y);
            ImGuizmo::SetOrthographic(false);
            ImGuizmo::SetDrawlist();

            glm::mat4 world_matrix = obj->world_matrix_;

            ImGuizmo::OPERATION op = ImGuizmo::TRANSLATE;
            if (gizmo_op_ == 1) op = ImGuizmo::ROTATE;
            if (gizmo_op_ == 2) op = ImGuizmo::SCALE;

            ImGuizmo::Manipulate(
                glm::value_ptr(view),
                glm::value_ptr(projection),
                op,
                ImGuizmo::LOCAL,
                glm::value_ptr(world_matrix));

            if (ImGuizmo::IsUsing()) {
                // Convert manipulated world matrix back to local space
                glm::mat4 local_matrix = world_matrix;
                if (obj->parent_id != -1) {
                    const auto* parent = scene_.find(
                        static_cast<uint32_t>(obj->parent_id));
                    if (parent) {
                        local_matrix = glm::inverse(parent->world_matrix_) * world_matrix;
                    }
                }
                obj->transform = decompose_matrix(local_matrix);
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

void Engine::draw_hierarchy() {
    if (ImGui::Begin("Hierarchy")) {
        remove_pending_id_ = 0;

        // Draw root objects
        for (auto& obj : scene_.objects()) {
            if (obj.parent_id == -1) {
                draw_hierarchy_node(obj);
            }
        }

        // Empty space = drop target for unparent to root
        ImVec2 avail = ImGui::GetContentRegionAvail();
        if (avail.y > 0) {
            ImGui::Dummy(avail);
            if (ImGui::BeginDragDropTarget()) {
                if (const auto* payload = ImGui::AcceptDragDropPayload("HIERARCHY_NODE")) {
                    uint32_t dragged_id = *static_cast<const uint32_t*>(payload->Data);
                    scene_.reparent(dragged_id, -1);
                }
                ImGui::EndDragDropTarget();
            }
        }

        // Deferred removal
        if (remove_pending_id_ != 0) {
            vulkan_context_.wait_idle();
            if (selected_id_ == remove_pending_id_) selected_id_ = 0;
            scene_.remove(remove_pending_id_);
        }
    }
    ImGui::End();
}

void Engine::draw_hierarchy_node(RenderObject& obj) {
    auto& objects = scene_.objects();

    bool has_children = false;
    for (const auto& other : objects) {
        if (other.parent_id == static_cast<int32_t>(obj.id)) {
            has_children = true;
            break;
        }
    }

    ImGui::PushID(static_cast<int>(obj.id));

    ImGuiTreeNodeFlags flags =
        ImGuiTreeNodeFlags_OpenOnArrow |
        ImGuiTreeNodeFlags_SpanAvailWidth |
        ImGuiTreeNodeFlags_DefaultOpen;

    if (!has_children) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }
    if (obj.id == selected_id_) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    bool node_open = ImGui::TreeNodeEx(obj.name.c_str(), flags);

    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        selected_id_ = obj.id;
    }

    // Drag source
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        ImGui::SetDragDropPayload("HIERARCHY_NODE", &obj.id, sizeof(obj.id));
        ImGui::Text("%s", obj.name.c_str());
        ImGui::EndDragDropSource();
    }

    // Drop target — reparent dragged object under this one
    if (ImGui::BeginDragDropTarget()) {
        if (const auto* payload = ImGui::AcceptDragDropPayload("HIERARCHY_NODE")) {
            uint32_t dragged_id = *static_cast<const uint32_t*>(payload->Data);
            if (dragged_id != obj.id) {
                // Cycle check: walk up from this node, ensure dragged isn't an ancestor
                bool is_ancestor = false;
                int32_t check = static_cast<int32_t>(obj.id);
                while (check != -1) {
                    if (static_cast<uint32_t>(check) == dragged_id) {
                        is_ancestor = true;
                        break;
                    }
                    const auto* p = scene_.find(static_cast<uint32_t>(check));
                    check = p ? p->parent_id : -1;
                }
                if (!is_ancestor) {
                    scene_.reparent(dragged_id, static_cast<int32_t>(obj.id));
                }
            }
        }
        ImGui::EndDragDropTarget();
    }

    // Delete button
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 20);
    if (ImGui::SmallButton("X")) {
        remove_pending_id_ = obj.id;
    }

    // Recurse children
    if (has_children && node_open) {
        for (auto& child : objects) {
            if (child.parent_id == static_cast<int32_t>(obj.id)) {
                draw_hierarchy_node(child);
            }
        }
        ImGui::TreePop();
    }

    ImGui::PopID();
}

void Engine::draw_properties() {
    if (ImGui::Begin("Properties")) {
        auto* obj = scene_.find(selected_id_);
        if (obj) {

            // Editable name
            char name_buf[128];
            std::strncpy(name_buf, obj->name.c_str(), sizeof(name_buf) - 1);
            name_buf[sizeof(name_buf) - 1] = '\0';
            if (ImGui::InputText("Name", name_buf, sizeof(name_buf))) {
                obj->name = name_buf;
            }

            ImGui::Separator();
            ImGui::TextColored({0.345f, 0.396f, 0.949f, 1.0f}, "Transform");
            ImGui::DragFloat3("Position", &obj->transform.position.x, 0.1f);
            ImGui::DragFloat3("Rotation", &obj->transform.rotation.x, 1.0f);
            ImGui::DragFloat3("Scale", &obj->transform.scale.x, 0.05f, 0.01f, 100.0f);

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextColored({0.345f, 0.396f, 0.949f, 1.0f}, "Gizmo");
            ImGui::RadioButton("Move (1)", &gizmo_op_, 0); ImGui::SameLine();
            ImGui::RadioButton("Rot (2)", &gizmo_op_, 1); ImGui::SameLine();
            ImGui::RadioButton("Scl (3)", &gizmo_op_, 2);

            ImGui::Spacing();
            ImGui::TextDisabled("Source: %s", obj->source.c_str());
        } else {
            ImGui::TextDisabled("No object selected");
        }

        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Lighting", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::DragFloat3("Direction", &light_.direction.x, 0.01f, -1.0f, 1.0f);
            ImGui::ColorEdit3("Color", &light_.color.x);
            ImGui::DragFloat("Intensity", &light_.intensity, 0.05f, 0.0f, 10.0f);
            ImGui::Spacing();
            ImGui::ColorEdit3("Ambient", &light_.ambient_color.x);
            ImGui::DragFloat("Ambient Intensity", &light_.ambient_intensity, 0.05f, 0.0f, 2.0f);
        }
    }
    ImGui::End();
}

void Engine::draw_assets() {
    if (ImGui::Begin("Assets")) {
        ImGui::TextColored({0.345f, 0.396f, 0.949f, 1.0f}, "Primitives");
        ImGui::Separator();
        if (ImGui::Button("Cube", {-1, 0})) add_primitive_to_scene("cube");
        if (ImGui::Button("Plane", {-1, 0})) add_primitive_to_scene("plane");
        if (ImGui::Button("Sphere", {-1, 0})) add_primitive_to_scene("sphere");

        ImGui::Spacing();
        ImGui::Spacing();

        ImGui::TextColored({0.345f, 0.396f, 0.949f, 1.0f}, "Models");
        ImGui::Separator();

        if (available_models_.empty()) {
            ImGui::TextDisabled("No models found");
        } else {
            for (const auto& model_path : available_models_) {
                auto name = std::filesystem::path(model_path).filename().string();
                if (ImGui::Button(name.c_str(), {-1, 0})) {
                    add_model_to_scene(model_path);
                }
            }
        }

        if (ImGui::Button("Refresh", {-1, 0})) {
            scan_models();
        }

        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Separator();

        // Scenes section (when project is open)
        if (project_ && !available_scenes_.empty()) {
            ImGui::Spacing();
            ImGui::Spacing();
            ImGui::TextColored({0.345f, 0.396f, 0.949f, 1.0f}, "Scenes");
            ImGui::Separator();

            for (const auto& scene_path : available_scenes_) {
                auto scene_name = std::filesystem::path(scene_path).stem().string();
                bool is_current = (scene_path == current_scene_path_);
                if (is_current) {
                    ImGui::PushStyleColor(ImGuiCol_Button, {0.2f, 0.4f, 0.8f, 1.0f});
                }
                if (ImGui::Button(scene_name.c_str(), {-1, 0})) {
                    load_scene(scene_path);
                }
                if (is_current) {
                    ImGui::PopStyleColor();
                }
            }
        }
    }
    ImGui::End();
}

void Engine::draw_stats() {
    // FPS calculation
    frame_count_++;
    fps_timer_ += clock_.delta_time();
    if (fps_timer_ >= 0.5f) {
        fps_ = static_cast<float>(frame_count_) / fps_timer_;
        frame_time_ = fps_timer_ / static_cast<float>(frame_count_) * 1000.0f;
        frame_count_ = 0;
        fps_timer_ = 0.0f;
    }

    frame_time_history_[frame_time_index_] = clock_.delta_time() * 1000.0f;
    frame_time_index_ = (frame_time_index_ + 1) % FRAME_TIME_HISTORY_SIZE;

    const auto& stats = vulkan_context_.render_stats();

    if (ImGui::Begin("Stats")) {
        ImGui::Text("GPU: %s", vulkan_context_.gpu_name().c_str());
        ImGui::SameLine(0, 20);
        ImGui::Text("FPS: %.0f", fps_);
        ImGui::SameLine(0, 20);
        ImGui::Text("Frame: %.2f ms", frame_time_);
        ImGui::SameLine(0, 20);
        ImGui::Text("Draw calls: %u", stats.draw_calls);
        ImGui::SameLine(0, 20);
        ImGui::Text("Triangles: %u", stats.triangle_count);

        ImGui::PlotLines("##FrameTime", frame_time_history_.data(),
                         FRAME_TIME_HISTORY_SIZE, frame_time_index_,
                         nullptr, 0.0f, 33.3f,
                         {ImGui::GetContentRegionAvail().x, 35});
    }
    ImGui::End();
}

void Engine::open_project(const std::filesystem::path& path) {
    auto proj = Project::open(path);
    if (!proj) return;

    project_ = std::move(proj);
    window_.set_title("Fjell - " + project_->config().name);
    scan_models();
    scan_scenes();
}

void Engine::close_project() {
    project_.reset();
    available_scenes_.clear();
    current_scene_path_ = base_path::resolve(SCENE_FILE);
    window_.set_title("Fjell Engine");
    scan_models();
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
}

void Engine::draw_menu_bar() {
    static char project_path_buf[512] = "";
    static char new_project_path_buf[512] = "";
    static char new_project_name_buf[128] = "";
    static bool show_open_popup = false;
    static bool show_new_popup = false;

    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Save Scene")) {
                scene_.save(current_scene_path_);
            }
            if (ImGui::MenuItem("Load Scene") && !current_scene_path_.empty()) {
                load_scene(current_scene_path_);
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Project")) {
            if (ImGui::MenuItem("New Project...")) {
                show_new_popup = true;
                new_project_path_buf[0] = '\0';
                new_project_name_buf[0] = '\0';
            }
            if (ImGui::MenuItem("Open Project...")) {
                show_open_popup = true;
                project_path_buf[0] = '\0';
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

        ImGui::EndMainMenuBar();
    }

    // Open Project popup
    if (show_open_popup) {
        ImGui::OpenPopup("Open Project");
        show_open_popup = false;
    }
    if (ImGui::BeginPopupModal("Open Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Project directory:");
        ImGui::InputText("##path", project_path_buf, sizeof(project_path_buf));

        if (ImGui::Button("Open", {120, 0})) {
            open_project(project_path_buf);
            // Reload scene from new project
            if (project_) {
                current_scene_path_ = project_->resolve(project_->config().default_scene);
                if (std::filesystem::exists(current_scene_path_)) {
                    load_scene(current_scene_path_);
                } else {
                    scene_.clear();
                    create_default_scene();
                }
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {120, 0})) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // New Project popup
    if (show_new_popup) {
        ImGui::OpenPopup("New Project");
        show_new_popup = false;
    }
    if (ImGui::BeginPopupModal("New Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Project name:");
        ImGui::InputText("##name", new_project_name_buf, sizeof(new_project_name_buf));
        ImGui::Text("Directory:");
        ImGui::InputText("##dir", new_project_path_buf, sizeof(new_project_path_buf));

        if (ImGui::Button("Create", {120, 0})) {
            if (new_project_name_buf[0] != '\0' && new_project_path_buf[0] != '\0') {
                project_ = Project::create(new_project_path_buf, new_project_name_buf);
                window_.set_title("Fjell - " + project_->config().name);
                scan_models();
                scan_scenes();
                current_scene_path_ = project_->resolve(project_->config().default_scene);
                scene_.clear();
                create_default_scene();
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {120, 0})) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void Engine::draw_ui() {
    // Gizmo keyboard shortcuts (only when ImGui doesn't want keyboard)
    if (!ImGui::GetIO().WantCaptureKeyboard) {
        if (ImGui::IsKeyPressed(ImGuiKey_1)) gizmo_op_ = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_2)) gizmo_op_ = 1;
        if (ImGui::IsKeyPressed(ImGuiKey_3)) gizmo_op_ = 2;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) selected_id_ = 0;
    }

    draw_menu_bar();
    setup_dockspace();
    draw_viewport();
    draw_hierarchy();
    draw_properties();
    draw_assets();
    draw_stats();
}

void Engine::start_play() {
    // Derive fly camera orientation from orbit camera
    // Orbit yaw is the same; negate pitch (orbit elevation → fly look angle)
    play_session_ = std::make_unique<PlaySession>(
        scene_, camera_.position(), camera_.yaw(), -camera_.pitch());
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
    play_session_->restore(scene_);
    input_.set_cursor_captured(window_.handle(), false);
    play_session_.reset();
    FJELL_CORE_INFO("Play mode stopped");
}

void Engine::draw_play_overlay() {
    // Resize viewport to fill window
    auto w = window_.width();
    auto h = window_.height();
    if (w > 0 && h > 0) {
        vulkan_context_.resize_viewport(w, h);
    }

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

    // Scene image fills the window
    auto tex_id = vulkan_context_.viewport_texture();
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
            play_session_->update(input_, dt);
            if (play_session_->wants_stop(input_)) {
                stop_play();
            }
        } else {
            if (!ImGuizmo::IsUsing()) {
                camera_.update(input_, dt);
            }
        }

        scene_.compute_world_matrices();

        imgui_layer_->begin_frame();
        if (is_playing()) {
            draw_play_overlay();
        } else {
            draw_ui();
        }
        imgui_layer_->end_frame();

        glm::mat4 view;
        glm::mat4 projection;
        glm::vec3 camera_pos;

        float aspect = static_cast<float>(window_.width()) /
                       static_cast<float>(std::max(window_.height(), 1u));
        if (is_playing()) {
            view = play_session_->camera().view_matrix();
            projection = play_session_->camera().projection_matrix(aspect);
            camera_pos = play_session_->camera().position();
        } else {
            view = camera_.view_matrix();
            projection = camera_.projection_matrix(aspect);
            camera_pos = camera_.position();
        }

        vulkan_context_.draw_frame(view, projection, camera_pos, light_, &scene_);
    }
    FJELL_CORE_INFO("Exiting main loop");
}

} // namespace fjell
