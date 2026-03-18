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
        vulkan_context_.bindless_textures(), vulkan_context_.mega_buffer());

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

    viewport_manager_.init(vulkan_context_.device_ref(), vulkan_context_.msaa_samples());
    viewport_manager_.add_viewport(ViewportSource::final_color, "Viewport");

    FJELL_CORE_INFO("Engine initialized");
}

Engine::~Engine() {
    vulkan_context_.wait_idle();
    vulkan_context_.set_overlay_callback(nullptr);
    scene_ = {};
    resource_manager_.reset();
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

    // Build default layout on first run (skip if restoring a saved layout)
    if (!layout_built_) {
        layout_built_ = true;
        if (has_saved_layout_) {
            ImGui::End();
            return;
        }

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
        if (auto* cam = viewport_manager_.active_camera()) {
            cam->focus(center, radius);
        }
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
                scene_.mark_dirty();
            }

            ImGui::Separator();
            ImGui::TextColored({0.345f, 0.396f, 0.949f, 1.0f}, "Transform");
            ImGui::Text("Position");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat3("##Position", &obj->transform.position.x, 0.1f)) scene_.mark_dirty();
            ImGui::Text("Rotation");
            ImGui::SetNextItemWidth(-1);
            {
                glm::vec3 euler = obj->transform.euler_degrees();
                if (ImGui::DragFloat3("##Rotation", &euler.x, 1.0f)) {
                    obj->transform.set_euler_degrees(euler);
                    scene_.mark_dirty();
                }
            }
            ImGui::Text("Scale");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat3("##Scale", &obj->transform.scale.x, 0.05f, 0.01f, 100.0f)) scene_.mark_dirty();

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextColored({0.345f, 0.396f, 0.949f, 1.0f}, "Gizmo");
            ImGui::RadioButton("Move (W)", &gizmo_op_, 0); ImGui::SameLine();
            ImGui::RadioButton("Rot (E)", &gizmo_op_, 1); ImGui::SameLine();
            ImGui::RadioButton("Scl (R)", &gizmo_op_, 2);

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

            // Particle Emitter
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextColored({0.345f, 0.396f, 0.949f, 1.0f}, "Particle Emitter");
            if (obj->emitter) {
                auto& em = *obj->emitter;
                if (ImGui::DragFloat("Emit Rate", &em.emission_rate, 1.0f, 0.0f, 10000.0f)) scene_.mark_dirty();
                if (ImGui::DragFloat("Lifetime Min", &em.lifetime_min, 0.1f, 0.01f, 30.0f)) scene_.mark_dirty();
                if (ImGui::DragFloat("Lifetime Max", &em.lifetime_max, 0.1f, 0.01f, 30.0f)) scene_.mark_dirty();
                int max_p = static_cast<int>(em.max_particles);
                if (ImGui::DragInt("Max Particles", &max_p, 100, 100, 100000)) {
                    em.max_particles = static_cast<uint32_t>(max_p);
                    scene_.mark_dirty();
                }
                if (ImGui::DragFloat3("Velocity Min", &em.velocity_min.x, 0.1f)) scene_.mark_dirty();
                if (ImGui::DragFloat3("Velocity Max", &em.velocity_max.x, 0.1f)) scene_.mark_dirty();
                if (ImGui::DragFloat3("Gravity", &em.gravity.x, 0.1f)) scene_.mark_dirty();
                if (ImGui::ColorEdit4("Color Start", &em.color_start.r)) scene_.mark_dirty();
                if (ImGui::ColorEdit4("Color End", &em.color_end.r)) scene_.mark_dirty();
                if (ImGui::DragFloat("Size Start", &em.size_start, 0.01f, 0.001f, 10.0f)) scene_.mark_dirty();
                if (ImGui::DragFloat("Size End", &em.size_end, 0.01f, 0.001f, 10.0f)) scene_.mark_dirty();
                static const char* blend_labels[] = {"Additive", "Alpha"};
                int bm = static_cast<int>(em.blend_mode);
                if (ImGui::Combo("Blend Mode", &bm, blend_labels, 2)) {
                    em.blend_mode = static_cast<ParticleBlendMode>(bm);
                    scene_.mark_dirty();
                }
                if (ImGui::SmallButton("Remove Emitter")) {
                    obj->emitter.reset();
                    scene_.mark_dirty();
                }
            } else {
                if (ImGui::Button("Add Particle Emitter", {-1, 0})) {
                    obj->emitter = ParticleEmitter{};
                    scene_.mark_dirty();
                }
            }

            // Components
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextColored({0.345f, 0.396f, 0.949f, 1.0f}, "Components");
            script_manager_.draw_inspector(*obj);

            // Add component button
            if (script_manager_.is_loaded() && !script_manager_.component_types().empty()) {
                if (ImGui::Button("Add Component", {-1, 0})) {
                    ImGui::OpenPopup("##AddComponent");
                }
                if (ImGui::BeginPopup("##AddComponent")) {
                    for (const auto& type : script_manager_.component_types()) {
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
        if (ImGui::CollapsingHeader("Directional Light", ImGuiTreeNodeFlags_DefaultOpen)) {
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

        if (ImGui::CollapsingHeader("Point Lights", ImGuiTreeNodeFlags_DefaultOpen)) {
            auto& point_lights = scene_.point_lights();
            int remove_idx = -1;
            for (int i = 0; i < static_cast<int>(point_lights.size()); ++i) {
                ImGui::PushID(i);
                auto& pl = point_lights[i];
                bool open = ImGui::TreeNode("##pl", "Point Light %d", i);
                if (open) {
                    ImGui::DragFloat3("Position", &pl.position.x, 0.1f);
                    ImGui::ColorEdit3("Color", &pl.color.x);
                    ImGui::DragFloat("Intensity", &pl.intensity, 0.1f, 0.0f, 100.0f);
                    ImGui::DragFloat("Radius", &pl.radius, 0.5f, 0.1f, 500.0f);
                    if (ImGui::SmallButton("Remove")) remove_idx = i;
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            if (remove_idx >= 0) {
                point_lights.erase(point_lights.begin() + remove_idx);
                scene_.mark_dirty();
            }
            if (ImGui::Button("Add Point Light", {-1, 0})) {
                scene_.add_point_light();
            }
        }

        if (ImGui::CollapsingHeader("Spot Lights")) {
            auto& spot_lights = scene_.spot_lights();
            int remove_idx = -1;
            for (int i = 0; i < static_cast<int>(spot_lights.size()); ++i) {
                ImGui::PushID(1000 + i);
                auto& sl = spot_lights[i];
                bool open = ImGui::TreeNode("##sl", "Spot Light %d", i);
                if (open) {
                    ImGui::DragFloat3("Position", &sl.position.x, 0.1f);
                    ImGui::DragFloat3("Direction", &sl.direction.x, 0.01f, -1.0f, 1.0f);
                    ImGui::ColorEdit3("Color", &sl.color.x);
                    ImGui::DragFloat("Intensity", &sl.intensity, 0.1f, 0.0f, 100.0f);
                    ImGui::DragFloat("Radius", &sl.radius, 0.5f, 0.1f, 500.0f);
                    ImGui::DragFloat("Inner Angle", &sl.inner_angle, 0.5f, 0.0f, 90.0f);
                    ImGui::DragFloat("Outer Angle", &sl.outer_angle, 0.5f, 0.0f, 90.0f);
                    if (ImGui::SmallButton("Remove")) remove_idx = i;
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            if (remove_idx >= 0) {
                spot_lights.erase(spot_lights.begin() + remove_idx);
                scene_.mark_dirty();
            }
            if (ImGui::Button("Add Spot Light", {-1, 0})) {
                scene_.add_spot_light();
            }
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
        ImGui::SameLine(0, 20);
        ImGui::Text("Culled: %u/%u", stats.objects_culled, stats.objects_total);
        ImGui::SameLine(0, 20);
        ImGui::Text("MSAA: %ux", static_cast<uint32_t>(vulkan_context_.msaa_samples()));

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

    // Defer layout restore to the first frame (ImGui needs a frame to set up docking)
    if (!project_->config().layout.empty()) {
        auto layout_path = project_->root() / "layouts" / (project_->config().layout + ".ini");
        if (std::filesystem::exists(layout_path)) {
            pending_layout_load_ = layout_path.string();
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

        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Add Viewport")) {
                viewport_manager_.add_viewport();
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Layout")) {
            // Save current layout
            if (ImGui::MenuItem("Save Layout...")) {
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
                    if (ImGui::MenuItem(name.c_str())) {
                        ImGui::LoadIniSettingsFromDisk(entry.path().string().c_str());
                    }
                }
            }
            ImGui::EndMenu();
        }

        if (project_ && ImGui::BeginMenu("Scripts")) {
            if (ImGui::MenuItem("New Component...")) {
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

    // Save Layout popup
    if (show_save_layout_popup_) {
        ImGui::OpenPopup("Save Layout");
        show_save_layout_popup_ = false;
    }
    if (ImGui::BeginPopupModal("Save Layout", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
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
            if (project_) {
                project_->config().layout = layout_name_buf_;
                project_->save();
            }
            FJELL_CORE_INFO("Layout saved: {}", path.string());
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // New Component script popup
    if (show_new_script_popup_) {
        ImGui::OpenPopup("New Component");
        show_new_script_popup_ = false;
    }
    if (ImGui::BeginPopupModal("New Component", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
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
                     << "class " << name << " : public fjell::Component {\n"
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
        scene_.save(current_scene_path_);
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
    auto* cam = viewport_manager_.active_camera();
    Camera fallback_cam;
    if (!cam) cam = &fallback_cam;
    play_session_ = std::make_unique<PlaySession>(
        scene_, cam->position(), cam->yaw() + 180.0f, -cam->pitch());
    script_manager_.prepare(scene_, input_, clock_, play_session_->physics_world());
    script_manager_.deserialize_all(scene_);
    script_manager_.call_begin_play(scene_);
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
            play_session_->update(input_, dt);
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

        // In play mode, override the active viewport's camera with the play camera
        if (is_playing() && !requests.empty()) {
            float aspect = viewport_manager_.active_aspect();
            requests[0].view = play_session_->camera().view_matrix();
            requests[0].projection = play_session_->camera().projection_matrix(aspect);
            requests[0].camera_position = play_session_->camera().position();
        }

        vulkan_context_.draw_frame(requests, light_, &scene_, clock_.delta_time(), selected_id_, thread_pool_);
    }
    FJELL_CORE_INFO("Exiting main loop");
}

} // namespace fjell
