#include "core/engine.hpp"
#include "core/base_path.hpp"
#include "core/game_builder.hpp"
#include "core/layer.hpp"
#include "core/log.hpp"
#include "physics/physics_world.hpp"
#include "ui/console.hpp"
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

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>

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
        vulkan_context_.swapchain_format(), vulkan_context_.image_count(),
        base_path::resolve("assets/fonts"));

    vulkan_context_.set_overlay_callback([this](VkCommandBuffer cmd) {
        imgui_layer_->render(cmd);
    });

    resource_manager_ = std::make_unique<ResourceManager>(
        vulkan_context_.device(), vulkan_context_.physical_device(),
        vulkan_context_.command_pool(), vulkan_context_.graphics_queue(),
        vulkan_context_.material_pool(), vulkan_context_.material_layout());

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

    layer_stack_.push_layer(std::make_unique<ViewportLayer>(&viewport_hovered_));

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

    // Build default layout on first run
    if (!layout_built_) {
        layout_built_ = true;

        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->WorkSize);

        // Viewport is the parent — split everything else off from it
        ImGuiID viewport_node = dockspace_id;

        // Split bottom strip (Stats/Console) off viewport
        ImGuiID bottom = ImGui::DockBuilderSplitNode(viewport_node, ImGuiDir_Down, 0.18f, nullptr, &viewport_node);

        // Split left panel (Assets) off viewport
        ImGuiID left = ImGui::DockBuilderSplitNode(viewport_node, ImGuiDir_Left, 0.18f, nullptr, &viewport_node);

        // Split right panel off viewport, then split that into top/bottom
        ImGuiID right = ImGui::DockBuilderSplitNode(viewport_node, ImGuiDir_Right, 0.22f, nullptr, &viewport_node);
        ImGuiID right_bottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.50f, nullptr, &right);

        // Split Code Editor below viewport
        ImGuiID editor = ImGui::DockBuilderSplitNode(viewport_node, ImGuiDir_Down, 0.40f, nullptr, &viewport_node);

        ImGui::DockBuilderDockWindow("Viewport", viewport_node);
        ImGui::DockBuilderDockWindow("Code Editor", editor);
        ImGui::DockBuilderDockWindow("Assets", left);
        ImGui::DockBuilderDockWindow("Hierarchy", right);
        ImGui::DockBuilderDockWindow("Properties", right_bottom);
        ImGui::DockBuilderDockWindow("Stats", bottom);
        ImGui::DockBuilderDockWindow("Console", bottom);

        ImGui::DockBuilderFinish(dockspace_id);
        focus_viewport_ = true;
    }

    ImGui::End();
}

void Engine::draw_viewport() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.0f, 0.0f, 0.0f, 0.0f});

    if (ImGui::Begin("Viewport")) {
        viewport_hovered_ = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);

        // Play button toolbar
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {4.0f, 4.0f});
        if (ImGui::Button(is_playing() ? "Stop" : "Play")) {
            if (is_playing()) stop_play();
            else start_play();
        }
        ImGui::SameLine();
        bool show_col = vulkan_context_.show_colliders();
        if (ImGui::Checkbox("Colliders", &show_col)) {
            vulkan_context_.set_show_colliders(show_col);
        }
        ImGui::PopStyleVar();

        auto cursor = ImGui::GetCursorScreenPos();
        auto vp_max = ImGui::GetWindowContentRegionMax();
        auto vp_pos = ImGui::GetWindowPos();
        auto vp_min = ImGui::GetWindowContentRegionMin();
        viewport_pos_ = {cursor.x, cursor.y};
        viewport_size_ = {vp_max.x - vp_min.x, vp_pos.y + vp_max.y - cursor.y};

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

        // Mouse picking — click/double-click objects in viewport
        if (ImGui::IsItemHovered() && !ImGuizmo::IsUsing() &&
            (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
             ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))) {
            // Build ray from mouse position through viewport
            auto mouse = ImGui::GetMousePos();
            float mx = (mouse.x - viewport_pos_.x) / viewport_size_.x;
            float my = (mouse.y - viewport_pos_.y) / viewport_size_.y;

            float aspect = viewport_size_.x / std::max(viewport_size_.y, 1.0f);
            auto view = camera_.view_matrix();
            auto proj = glm::perspective(
                glm::radians(camera_.fov()), aspect, 0.1f, 1000.0f);
            auto inv_vp = glm::inverse(proj * view);

            // NDC: [-1,1] range, Y flipped for Vulkan projection
            glm::vec4 ndc_near{mx * 2.0f - 1.0f, my * 2.0f - 1.0f, 0.0f, 1.0f};
            glm::vec4 ndc_far {mx * 2.0f - 1.0f, my * 2.0f - 1.0f, 1.0f, 1.0f};

            glm::vec4 world_near = inv_vp * ndc_near;
            glm::vec4 world_far  = inv_vp * ndc_far;
            world_near /= world_near.w;
            world_far  /= world_far.w;

            glm::vec3 ray_origin = glm::vec3(world_near);
            glm::vec3 ray_dir = glm::normalize(glm::vec3(world_far - world_near));

            // Test ray against each object's world-space AABB
            float closest_t = std::numeric_limits<float>::max();
            uint32_t hit_id = 0;

            for (const auto& obj : scene_.objects()) {
                if (!obj.model) continue;
                const auto& verts = obj.model->collision_vertices();
                if (verts.empty()) continue;

                // Compute world-space AABB
                glm::vec3 aabb_min{std::numeric_limits<float>::max()};
                glm::vec3 aabb_max{std::numeric_limits<float>::lowest()};
                for (const auto& v : verts) {
                    glm::vec3 world_v = glm::vec3(obj.world_matrix_ * glm::vec4(v, 1.0f));
                    aabb_min = glm::min(aabb_min, world_v);
                    aabb_max = glm::max(aabb_max, world_v);
                }

                // Ray-AABB intersection (slab method)
                glm::vec3 inv_dir = 1.0f / ray_dir;
                glm::vec3 t1 = (aabb_min - ray_origin) * inv_dir;
                glm::vec3 t2 = (aabb_max - ray_origin) * inv_dir;
                glm::vec3 t_min_v = glm::min(t1, t2);
                glm::vec3 t_max_v = glm::max(t1, t2);
                float t_enter = std::max({t_min_v.x, t_min_v.y, t_min_v.z});
                float t_exit  = std::min({t_max_v.x, t_max_v.y, t_max_v.z});

                if (t_enter <= t_exit && t_exit >= 0.0f) {
                    float t = t_enter >= 0.0f ? t_enter : t_exit;
                    if (t < closest_t) {
                        closest_t = t;
                        hit_id = obj.id;
                    }
                }
            }

            if (hit_id != 0) {
                selected_id_ = hit_id;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    if (auto* hit_obj = scene_.find(hit_id)) {
                        glm::vec3 center = glm::vec3(hit_obj->world_matrix_[3]);
                        float radius = 1.0f;
                        if (hit_obj->model && !hit_obj->model->collision_vertices().empty()) {
                            float max_r2 = 0.0f;
                            for (const auto& v : hit_obj->model->collision_vertices()) {
                                auto wv = glm::vec3(hit_obj->world_matrix_ * glm::vec4(v, 1.0f));
                                max_r2 = std::max(max_r2, glm::dot(wv - center, wv - center));
                            }
                            radius = std::max(std::sqrt(max_r2), 0.1f);
                        }
                        camera_.focus(center, radius);
                    }
                }
            } else {
                selected_id_ = 0;
            }
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

    bool is_selected = (obj.id == selected_id_);

    ImGuiTreeNodeFlags flags =
        ImGuiTreeNodeFlags_OpenOnArrow |
        ImGuiTreeNodeFlags_SpanAvailWidth |
        ImGuiTreeNodeFlags_DefaultOpen |
        ImGuiTreeNodeFlags_FramePadding;

    if (!has_children) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }
    if (is_selected) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    // Row styling — equal vertical padding for centered text
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {4.0f, 4.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0.0f, 1.0f});

    if (is_selected) {
        ImGui::PushStyleColor(ImGuiCol_Header, {0.22f, 0.27f, 0.58f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, {0.26f, 0.32f, 0.65f, 1.0f});
    }

    bool node_open = ImGui::TreeNodeEx(obj.name.c_str(), flags);

    if (is_selected) {
        ImGui::PopStyleColor(2);
    }

    ImGui::PopStyleVar(2);

    bool row_hovered = ImGui::IsItemHovered();

    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        selected_id_ = obj.id;
    }
    if (row_hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        selected_id_ = obj.id;
        glm::vec3 center = glm::vec3(obj.world_matrix_[3]);
        float radius = 1.0f;
        if (obj.model && !obj.model->collision_vertices().empty()) {
            float max_r2 = 0.0f;
            for (const auto& v : obj.model->collision_vertices()) {
                auto wv = glm::vec3(obj.world_matrix_ * glm::vec4(v, 1.0f));
                max_r2 = std::max(max_r2, glm::dot(wv - center, wv - center));
            }
            radius = std::max(std::sqrt(max_r2), 0.1f);
        }
        camera_.focus(center, radius);
    }

    // Drag source — must follow immediately after TreeNodeEx
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

    // Delete button — always present to avoid layout shift, hidden when not relevant
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 20);
    if (row_hovered || is_selected) {
        ImGui::PushStyleColor(ImGuiCol_Button, {0.0f, 0.0f, 0.0f, 0.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.8f, 0.2f, 0.2f, 0.6f});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0.9f, 0.1f, 0.1f, 0.8f});
        ImGui::PushStyleColor(ImGuiCol_Text, {0.7f, 0.7f, 0.7f, 1.0f});
        if (ImGui::SmallButton("X")) {
            remove_pending_id_ = obj.id;
        }
        ImGui::PopStyleColor(4);
    } else {
        ImGui::Dummy({20.0f, 0.0f});
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
            ImGui::Text("Name");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##Name", name_buf, sizeof(name_buf))) {
                obj->name = name_buf;
            }

            ImGui::Separator();
            ImGui::TextColored({0.345f, 0.396f, 0.949f, 1.0f}, "Transform");
            ImGui::Text("Position");
            ImGui::SetNextItemWidth(-1);
            ImGui::DragFloat3("##Position", &obj->transform.position.x, 0.1f);
            ImGui::Text("Rotation");
            ImGui::SetNextItemWidth(-1);
            ImGui::DragFloat3("##Rotation", &obj->transform.rotation.x, 1.0f);
            ImGui::Text("Scale");
            ImGui::SetNextItemWidth(-1);
            ImGui::DragFloat3("##Scale", &obj->transform.scale.x, 0.05f, 0.01f, 100.0f);

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextColored({0.345f, 0.396f, 0.949f, 1.0f}, "Gizmo");
            ImGui::RadioButton("Move (1)", &gizmo_op_, 0); ImGui::SameLine();
            ImGui::RadioButton("Rot (2)", &gizmo_op_, 1); ImGui::SameLine();
            ImGui::RadioButton("Scl (3)", &gizmo_op_, 2);

            ImGui::Spacing();
            ImGui::TextDisabled("Source: %s", obj->source.c_str());

            // Rigidbody
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextColored({0.345f, 0.396f, 0.949f, 1.0f}, "Rigidbody");
            if (obj->rigidbody) {
                auto& rb = *obj->rigidbody;
                static const char* body_type_labels[] = {"Static", "Kinematic", "Dynamic"};
                int bt = static_cast<int>(rb.body_type);
                ImGui::Text("Body Type");
                ImGui::SetNextItemWidth(-1);
                if (ImGui::Combo("##BodyType", &bt, body_type_labels, 3)) {
                    rb.body_type = static_cast<BodyType>(bt);
                }
                ImGui::Text("Mass");
                ImGui::SetNextItemWidth(-1);
                ImGui::DragFloat("##Mass", &rb.mass, 0.1f, 0.01f, 1000.0f);
                ImGui::Text("Friction");
                ImGui::SetNextItemWidth(-1);
                ImGui::DragFloat("##Friction", &rb.friction, 0.01f, 0.0f, 2.0f);
                ImGui::Text("Restitution");
                ImGui::SetNextItemWidth(-1);
                ImGui::DragFloat("##Restitution", &rb.restitution, 0.01f, 0.0f, 2.0f);
                ImGui::Text("Linear Damping");
                ImGui::SetNextItemWidth(-1);
                ImGui::DragFloat("##LinearDamping", &rb.linear_damping, 0.01f, 0.0f, 10.0f);
                ImGui::Text("Angular Damping");
                ImGui::SetNextItemWidth(-1);
                ImGui::DragFloat("##AngularDamping", &rb.angular_damping, 0.01f, 0.0f, 10.0f);
                ImGui::Checkbox("Gravity", &rb.gravity_enabled);
                if (ImGui::SmallButton("Remove Rigidbody")) {
                    obj->rigidbody.reset();
                }
            } else {
                if (ImGui::Button("Add Rigidbody", {-1, 0})) {
                    obj->rigidbody = RigidbodyDesc{};
                }
            }

            // Collider
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextColored({0.345f, 0.396f, 0.949f, 1.0f}, "Collider");
            if (obj->collider) {
                auto& col = *obj->collider;
                static const char* shape_labels[] = {"Box", "Sphere", "Capsule", "Mesh"};
                int sh = static_cast<int>(col.shape);
                ImGui::Text("Shape");
                ImGui::SetNextItemWidth(-1);
                if (ImGui::Combo("##Shape", &sh, shape_labels, 4)) {
                    col.shape = static_cast<ColliderShape>(sh);
                }
                switch (col.shape) {
                    case ColliderShape::Box:
                        ImGui::Text("Half Extents");
                        ImGui::SetNextItemWidth(-1);
                        ImGui::DragFloat3("##HalfExtents", &col.half_extents.x, 0.05f, 0.01f, 100.0f);
                        break;
                    case ColliderShape::Sphere:
                        ImGui::Text("Radius");
                        ImGui::SetNextItemWidth(-1);
                        ImGui::DragFloat("##Radius", &col.radius, 0.05f, 0.01f, 100.0f);
                        break;
                    case ColliderShape::Capsule:
                        ImGui::Text("Radius");
                        ImGui::SetNextItemWidth(-1);
                        ImGui::DragFloat("##RadiusCap", &col.radius, 0.05f, 0.01f, 100.0f);
                        ImGui::Text("Height");
                        ImGui::SetNextItemWidth(-1);
                        ImGui::DragFloat("##Height", &col.height, 0.05f, 0.01f, 100.0f);
                        break;
                    case ColliderShape::Mesh:
                        ImGui::TextDisabled("Uses model collision mesh");
                        break;
                }
                ImGui::Text("Offset");
                ImGui::SetNextItemWidth(-1);
                ImGui::DragFloat3("##Offset", &col.offset.x, 0.05f);
                if (ImGui::SmallButton("Remove Collider")) {
                    obj->collider.reset();
                }
            } else {
                if (ImGui::Button("Add Collider", {-1, 0})) {
                    ColliderDesc col{};
                    // Auto-detect shape from primitive source
                    if (obj->source == "primitive:cube") {
                        col.shape = ColliderShape::Box;
                    } else if (obj->source == "primitive:sphere") {
                        col.shape = ColliderShape::Sphere;
                        col.radius = 0.5f;
                    } else {
                        col.shape = ColliderShape::Box;
                    }
                    obj->collider = col;
                }
            }

            // Components
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextColored({0.345f, 0.396f, 0.949f, 1.0f}, "Components");
            script_manager_.draw_inspector(*obj);

            // Add component button
            if (script_manager_.is_loaded() && !script_manager_.types().empty()) {
                if (ImGui::Button("Add Component", {-1, 0})) {
                    ImGui::OpenPopup("##AddComponent");
                }
                if (ImGui::BeginPopup("##AddComponent")) {
                    for (const auto& type : script_manager_.types()) {
                        if (type.loaded && ImGui::MenuItem(type.name.c_str())) {
                            script_manager_.add_component(*obj, type.name);
                        }
                    }
                    ImGui::EndPopup();
                }
            }
        } else {
            ImGui::TextDisabled("No object selected");
        }

        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Lighting", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Direction");
            ImGui::SetNextItemWidth(-1);
            ImGui::DragFloat3("##Direction", &light_.direction.x, 0.01f, -1.0f, 1.0f);
            ImGui::Text("Color");
            ImGui::SetNextItemWidth(-1);
            ImGui::ColorEdit3("##Color", &light_.color.x);
            ImGui::Text("Intensity");
            ImGui::SetNextItemWidth(-1);
            ImGui::DragFloat("##Intensity", &light_.intensity, 0.05f, 0.0f, 10.0f);
            ImGui::Spacing();
            ImGui::Text("Ambient");
            ImGui::SetNextItemWidth(-1);
            ImGui::ColorEdit3("##Ambient", &light_.ambient_color.x);
            ImGui::Text("Ambient Intensity");
            ImGui::SetNextItemWidth(-1);
            ImGui::DragFloat("##AmbientIntensity", &light_.ambient_intensity, 0.05f, 0.0f, 2.0f);
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

        // Scripts section (when project is open)
        if (project_) {
            ImGui::Spacing();
            ImGui::Spacing();
            ImGui::TextColored({0.345f, 0.396f, 0.949f, 1.0f}, "Scripts");
            ImGui::Separator();

            auto scripts_dir = std::filesystem::path(project_->resolve("scripts"));
            if (std::filesystem::exists(scripts_dir)) {
                for (const auto& entry : std::filesystem::directory_iterator(scripts_dir)) {
                    if (!entry.is_regular_file()) continue;
                    auto ext = entry.path().extension().string();
                    if (ext != ".cpp" && ext != ".h" && ext != ".hpp") continue;
                    auto fname = entry.path().filename().string();
                    if (ImGui::Selectable(fname.c_str())) {
                        nvim_panel_.open(entry.path().string());
                    }
                }
            } else {
                ImGui::TextDisabled("No scripts/ directory");
            }
        }

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

        ImGui::EndMainMenuBar();
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
            current_scene_path_ = project_->resolve(project_->config().default_scene);
            scene_.clear();
            create_default_scene();
        }
    }
}

void Engine::draw_ui() {
    // Gizmo keyboard shortcuts (only when viewport owns input)
    if (layer_stack_.has_input("Viewport")) {
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
    draw_console();
    draw_code_editor();

    if (focus_viewport_) {
        ImGui::SetWindowFocus("Viewport");
        focus_viewport_ = false;
    }
}

void Engine::start_play() {
    script_manager_.serialize_all(scene_);
    play_session_ = std::make_unique<PlaySession>(
        scene_, camera_.position(), camera_.yaw() + 180.0f, -camera_.pitch());
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
    script_manager_.deserialize_all(scene_);
    input_.set_cursor_captured(window_.handle(), false);
    play_session_.reset();
    focus_viewport_ = true;
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
            script_manager_.update(scene_, input_, clock_, play_session_->physics_world());
            play_session_->physics_step(dt, scene_);
            if (play_session_->wants_stop(input_)) {
                stop_play();
            }
        } else {
            if (!ImGuizmo::IsUsing() && viewport_hovered_) {
                camera_.update(input_, dt);
            }
        }

#ifndef NDEBUG
        (void)script_manager_.poll_reload(scene_);
#endif

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

        float aspect = viewport_size_.x / std::max(viewport_size_.y, 1.0f);
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
