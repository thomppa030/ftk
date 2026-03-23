#include "ui/scene_editor_context.hpp"
#include "core/base_path.hpp"
#include "core/clock.hpp"
#include "core/commands.hpp"
#include "core/frame_context.hpp"
#include "core/input.hpp"
#include "core/log.hpp"
#include "core/project_manager.hpp"
#include "core/window.hpp"
#include "animation/animation_clip.hpp"
#include "animation/animator_component.hpp"
#include "animation/skeleton.hpp"
#include "audio/audio_subsystem.hpp"
#include "physics/physics_subsystem.hpp"
#include "physics/physics_world.hpp"
#include "renderer/arrow_component.hpp"
#include "renderer/asset_loader.hpp"
#include "renderer/bindless.hpp"
#include "renderer/mesh_component.hpp"
#include "renderer/model.hpp"
#include "renderer/resource_manager.hpp"
#include "renderer/skeletal_mesh_component.hpp"
#include "renderer/skeletal_model.hpp"
#include "renderer/sprite_desc.hpp"
#include "renderer/texture.hpp"
#include "renderer/vulkan_context.hpp"
#include "renderer/vfx_subsystem.hpp"
#include "scene/camera_node.hpp"
#include "scene/light_node.hpp"
#include "scripting/script_subsystem.hpp"
#include "ui/game_ui/game_ui.hpp"
#include "ui/icon_cache.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <GLFW/glfw3.h>

#include <filesystem>

namespace fjell {

SceneEditorContext::SceneEditorContext(SceneEditorDeps deps)
    : deps_{deps} {}

void SceneEditorContext::init() {
    viewport_manager_.init(deps_.vulkan_context->device_ref(),
                           deps_.vulkan_context->msaa_samples());
    viewport_manager_.add_viewport(ViewportSource::final_color, "Viewport");

    hierarchy_panel_.init(deps_.scene, &viewport_manager_, &selected_id_,
                          &command_history(), deps_.icon_cache);
    properties_panel_.init(deps_.scene, &selected_id_, &gizmo_op_, deps_.light,
                           &deps_.script_subsystem->manager(), &command_history(),
                           &deps_.vulkan_context->grid_pass().settings(),
                           deps_.vulkan_context->pipeline_cache(),
                           &deps_.vulkan_context->bindless_textures());
    node_editor_panel_.init(deps_.scene, &selected_id_);

    // Bind context-level delegates
    connections_.push_back(hierarchy_panel_.on_add_primitive.bind(
        [this](const std::string& type) {
            add_primitive(type);
            command_history().execute(std::make_unique<AddObjectCommand>(
                *deps_.scene, selected_id_, &selected_id_));
        }));
    connections_.push_back(hierarchy_panel_.on_remove_object.bind(
        [this](uint32_t id) {
            command_history().execute(std::make_unique<RemoveObjectCommand>(
                *deps_.scene, id, &selected_id_,
                [this]() { deps_.vulkan_context->wait_idle(); }));
        }));
    connections_.push_back(hierarchy_panel_.on_duplicate.bind(
        [this]() { duplicate_selection(); }));
    connections_.push_back(viewport_manager_.on_play.bind(
        [this]() { start_play(); }));
    connections_.push_back(viewport_manager_.on_stop.bind(
        [this]() { stop_play(); }));
    connections_.push_back(viewport_manager_.on_pause.bind(
        []() { /* TODO: pause */ }));
}

void SceneEditorContext::shutdown() {
    node_editor_panel_.shutdown();
    viewport_manager_.release_textures();
    connections_.clear();
}

void SceneEditorContext::draw(float dt) {
    viewport_manager_.draw(*deps_.vulkan_context, *deps_.scene, selected_id_,
                           *deps_.input, dt, gizmo_op_, &command_history());
    hierarchy_panel_.draw();
    properties_panel_.draw();
    node_editor_panel_.draw();

    if (focus_viewport_) {
        ImGui::SetWindowFocus("Viewport");
        focus_viewport_ = false;
    }
}

void SceneEditorContext::setup_dockspace(ImGuiID main_area) {
    // Right panel: Hierarchy + Properties
    ImGuiID viewport_node = main_area;
    ImGuiID right = ImGui::DockBuilderSplitNode(viewport_node, ImGuiDir_Right,
                                                 0.22f, nullptr, &viewport_node);
    ImGuiID right_bottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down,
                                                        0.50f, nullptr, &right);

    ImGui::DockBuilderDockWindow("Viewport", viewport_node);
    ImGui::DockBuilderDockWindow("Hierarchy", right);
    ImGui::DockBuilderDockWindow("History", right);
    ImGui::DockBuilderDockWindow("Properties", right_bottom);
}

bool SceneEditorContext::handle_shortcuts() {
    if (ImGui::GetIO().WantTextInput) return false;

    bool navigating = deps_.input->is_mouse_button_held(GLFW_MOUSE_BUTTON_RIGHT);
    if (navigating) return false;

    // Gizmo mode shortcuts
    if (ImGui::IsKeyPressed(ImGuiKey_W)) { gizmo_op_ = 0; return true; }
    if (ImGui::IsKeyPressed(ImGuiKey_E)) { gizmo_op_ = 1; return true; }
    if (ImGui::IsKeyPressed(ImGuiKey_R)) { gizmo_op_ = 2; return true; }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        hierarchy_panel_.clear_selection();
        return true;
    }

    // Duplicate (Ctrl+D)
    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D)) {
        duplicate_selection();
        return true;
    }

    // Delete selection
    if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        delete_selection();
        return true;
    }

    return false;
}

// --- Play mode ---

void SceneEditorContext::start_play() {
    command_history().clear();
    deps_.script_subsystem->manager().serialize_all(*deps_.scene);
    play_session_ = std::make_unique<PlaySession>(*deps_.scene);

    // Create play-mode resources (physics first, scripts need it)
    {
        TickContext ctx{
            .scene = *deps_.scene,
            .input = *deps_.input,
            .clock = *deps_.clock,
            .dt = deps_.clock->delta_time(),
            .thread_pool = *deps_.thread_pool,
            .physics = deps_.physics_subsystem->world(),
            .audio = &deps_.audio_subsystem->engine(),
            .vfx = nullptr,
            .project = deps_.project_mgr->active(),
        };
        deps_.physics_subsystem->on_play_start(ctx);
        deps_.vfx_subsystem->on_play_start(ctx);
    }

    // Rebuild context now that physics exists
    {
        TickContext ctx{
            .scene = *deps_.scene,
            .input = *deps_.input,
            .clock = *deps_.clock,
            .dt = deps_.clock->delta_time(),
            .thread_pool = *deps_.thread_pool,
            .physics = deps_.physics_subsystem->world(),
            .audio = &deps_.audio_subsystem->engine(),
            .vfx = nullptr,
            .project = deps_.project_mgr->active(),
        };
        deps_.audio_subsystem->on_play_start(ctx);
        deps_.script_subsystem->on_play_start(ctx);
    }

    deps_.vulkan_context->set_playing(true);
    deps_.game_ui->mark_dirty();
    deps_.input->set_cursor_captured(deps_.window->handle(), true);
    selected_id_ = 0;
    FJELL_CORE_INFO("Play mode started");
}

void SceneEditorContext::stop_play() {
    if (deps_.game_mode) {
        glfwSetWindowShouldClose(deps_.window->handle(), GLFW_TRUE);
        return;
    }
    TickContext ctx{
        .scene = *deps_.scene,
        .input = *deps_.input,
        .clock = *deps_.clock,
        .dt = deps_.clock->delta_time(),
        .thread_pool = *deps_.thread_pool,
        .physics = deps_.physics_subsystem->world(),
        .audio = &deps_.audio_subsystem->engine(),
        .vfx = nullptr,
        .project = deps_.project_mgr->active(),
    };
    deps_.audio_subsystem->on_play_stop(ctx);
    deps_.script_subsystem->on_play_stop(ctx);
    deps_.vfx_subsystem->on_play_stop(ctx);

    play_session_->restore(*deps_.scene);
    deps_.script_subsystem->manager().deserialize_all(*deps_.scene);
    deps_.physics_subsystem->on_play_stop(ctx);

    deps_.vulkan_context->set_playing(false);
    deps_.input->set_cursor_captured(deps_.window->handle(), false);
    play_session_.reset();
    focus_viewport_ = true;
    FJELL_CORE_INFO("Play mode stopped");
}

bool SceneEditorContext::play_wants_stop() const {
    return play_session_ && play_session_->wants_stop(*deps_.input);
}

void SceneEditorContext::draw_play_overlay() {
    // In play mode, resize the active viewport's offscreen to fill the window
    auto w = deps_.window->width();
    auto h = deps_.window->height();
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
    auto tex_id = viewport_manager_.active_texture(*deps_.vulkan_context);
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

// --- Viewport ---

std::vector<ViewportRenderRequest> SceneEditorContext::build_render_requests() {
    return viewport_manager_.build_render_requests();
}

void SceneEditorContext::apply_resize(VulkanContext& vk) {
    viewport_manager_.apply_resize(vk);
}

bool SceneEditorContext::any_viewport_hovered() const {
    return viewport_manager_.any_hovered();
}

// --- Scene mutation ---

void SceneEditorContext::add_model(const std::string& path) {
    deps_.vulkan_context->wait_idle();

    auto model_result = deps_.resource_manager->load_model(
        base_path::resolve_asset(path, deps_.project_mgr->active()));
    if (!model_result) {
        FJELL_CORE_ERROR("{}", model_result.error());
        return;
    }
    auto model_handle = *model_result;

    auto tex = deps_.resource_manager->load_texture(
        base_path::resolve("engine_assets/textures/test.png")).value_or(TextureHandle{});
    auto mat_handle = deps_.resource_manager->create_material("default", tex);
    std::string mat_name = "default";

    auto* model = deps_.resource_manager->get(model_handle);
    if (model && !model->textures().empty()) {
        mat_name = path + "_mat";
        mat_handle = deps_.resource_manager->create_material_from_texture(
            mat_name,
            const_cast<Texture*>(model->textures()[0].get()));
    }

    auto filename = std::filesystem::path(path).stem().string();
    selected_id_ = deps_.scene->add(model_handle, mat_handle, *deps_.resource_manager,
                                     {}, filename, path, mat_name);
    FJELL_CORE_INFO("Added model to scene: {}", path);
}

void SceneEditorContext::add_skeletal_model(const std::string& mesh_path,
                                             const std::string& skel_path,
                                             const std::string& anim_path) {
    deps_.vulkan_context->wait_idle();

    auto resolved_mesh = base_path::resolve_asset(mesh_path, deps_.project_mgr->active());
    auto resolved_skel = base_path::resolve_asset(skel_path, deps_.project_mgr->active());

    auto skel_result = deps_.resource_manager->load_skeletal_model(resolved_mesh);
    if (!skel_result) {
        FJELL_CORE_ERROR("{}", skel_result.error());
        return;
    }
    auto skel_model_handle = *skel_result;
    auto* skel_model = deps_.resource_manager->get(skel_model_handle);
    if (!skel_model) {
        FJELL_CORE_ERROR("Failed to load skeletal model: {}", mesh_path);
        return;
    }

    // Create scene node with SkeletalMeshComponent
    auto filename = std::filesystem::path(mesh_path).stem().string();
    auto node = std::make_unique<SceneNode>();
    node->name = filename;
    node->source = mesh_path;

    // Set up skeletal mesh component
    auto& smc = node->components.add<SkeletalMeshComponent>();
    smc.skeletal_model_handle = skel_model_handle;
    smc.skeletal_model = skel_model;

    // Default material
    auto tex = deps_.resource_manager->load_texture(
        base_path::resolve("engine_assets/textures/test.png")).value_or(TextureHandle{});
    smc.material_handle = deps_.resource_manager->create_material("skel_default", tex);
    smc.material = deps_.resource_manager->get(smc.material_handle);

    // Load skeleton (owned by the component)
    smc.skeleton = std::make_shared<Skeleton>(Skeleton::load(resolved_skel));

    // Set up animator
    smc.animator = std::make_unique<AnimatorComponent>();
    smc.animator->init(smc.skeleton.get());

    // Play animation clip if provided
    if (!anim_path.empty()) {
        auto resolved_anim = base_path::resolve_asset(anim_path, deps_.project_mgr->active());
        auto clip = std::make_shared<AnimationClip>(AnimationClip::load(resolved_anim));
        smc.animator->play(clip.get());
        smc.clips.push_back(clip);
        FJELL_CORE_INFO("Playing animation: {}", anim_path);
    }

    selected_id_ = deps_.scene->add_node(std::move(node));
    FJELL_CORE_INFO("Added skeletal model to scene: {}", mesh_path);
}

void SceneEditorContext::add_primitive(const std::string& type) {
    deps_.vulkan_context->wait_idle();

    // Empty node: no mesh
    if (type == "empty") {
        selected_id_ = deps_.scene->add_empty();
        return;
    }

    // Folder: non-spatial organizational node
    if (type == "folder") {
        selected_id_ = deps_.scene->add_folder();
        return;
    }

    // Camera node
    if (type == "camera") {
        auto node = std::make_unique<CameraNode>();
        node->name = "Camera";
        auto& sp = node->components.add<SpriteComponent>();
        sp.texture_path = "engine_assets/icons/icon_camera.png";
        sp.editor_only = true;
        node->components.add<ArrowComponent>();
        selected_id_ = deps_.scene->add_node(std::move(node));
        return;
    }

    // Light nodes
    if (type == "light:directional") {
        auto node = std::make_unique<DirectionalLightNode>();
        node->name = "Directional Light";
        auto& sp = node->components.add<SpriteComponent>();
        sp.texture_path = "engine_assets/icons/icon_dir_light.png";
        sp.editor_only = true;
        selected_id_ = deps_.scene->add_node(std::move(node));
        return;
    }
    if (type == "light:point") {
        auto node = std::make_unique<PointLightNode>();
        node->name = "Point Light";
        auto& sp = node->components.add<SpriteComponent>();
        sp.texture_path = "engine_assets/icons/icon_point_light.png";
        sp.editor_only = true;
        selected_id_ = deps_.scene->add_node(std::move(node));
        return;
    }
    if (type == "light:spot") {
        auto node = std::make_unique<SpotLightNode>();
        node->name = "Spot Light";
        auto& sp = node->components.add<SpriteComponent>();
        sp.texture_path = "engine_assets/icons/icon_point_light.png";
        sp.editor_only = true;
        selected_id_ = deps_.scene->add_node(std::move(node));
        return;
    }
    if (type == "volumetric_fog") {
        auto node = std::make_unique<VolumetricFogNode>();
        node->name = "Volumetric Fog";
        selected_id_ = deps_.scene->add_node(std::move(node));
        return;
    }
    if (type == "world_environment") {
        auto node = std::make_unique<WorldEnvironmentNode>();
        node->name = "World Environment";
        selected_id_ = deps_.scene->add_node(std::move(node));
        return;
    }

    // Sprite objects: plane primitive with a sprite component
    if (type == "sprite") {
        auto model_result = deps_.resource_manager->create_primitive("plane");
        if (!model_result) return;

        auto tex = deps_.resource_manager->load_texture(
            base_path::resolve("engine_assets/textures/test.png")).value_or(TextureHandle{});
        auto mat = deps_.resource_manager->create_material("default", tex);

        selected_id_ = deps_.scene->add(*model_result, mat, *deps_.resource_manager,
                                         {}, "Sprite", "primitive:plane");
        auto* obj = deps_.scene->find(selected_id_);
        if (obj) {
            obj->components.add<SpriteComponent>();
        }
        return;
    }

    auto model_result = deps_.resource_manager->create_primitive(type);
    if (!model_result) return;

    auto tex = deps_.resource_manager->load_texture(
        base_path::resolve("engine_assets/textures/test.png")).value_or(TextureHandle{});
    auto mat = deps_.resource_manager->create_material("default", tex);

    std::string name = type;
    name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));

    selected_id_ = deps_.scene->add(*model_result, mat, *deps_.resource_manager,
                                     {}, name, "primitive:" + type);
}

void SceneEditorContext::duplicate_selection() {
    auto& sel = hierarchy_panel_.selection();
    if (sel.empty()) return;

    deps_.vulkan_context->wait_idle();
    std::vector<uint32_t> ids(sel.begin(), sel.end());
    auto cmd = std::make_unique<DuplicateCommand>(*deps_.scene, ids, &selected_id_);
    command_history().execute(std::move(cmd));

    // Update multi-selection to the new duplicates
    auto* dup_cmd = static_cast<DuplicateCommand*>(
        command_history().commands().back().get());
    hierarchy_panel_.clear_selection();
    for (uint32_t id : dup_cmd->created_ids()) {
        hierarchy_panel_.add_to_selection(id);
    }
}

void SceneEditorContext::delete_selection() {
    auto& sel = hierarchy_panel_.selection();
    if (sel.empty()) return;

    deps_.vulkan_context->wait_idle();
    // Delete in reverse order to avoid invalidating IDs
    std::vector<uint32_t> ids(sel.begin(), sel.end());
    for (uint32_t id : ids) {
        command_history().execute(std::make_unique<RemoveObjectCommand>(
            *deps_.scene, id, &selected_id_,
            [this]() { deps_.vulkan_context->wait_idle(); }));
    }
    hierarchy_panel_.clear_selection();
}

} // namespace fjell
