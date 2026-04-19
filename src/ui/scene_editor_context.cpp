#include "ui/scene_editor_context.hpp"
#include "ui/dockspace_layout.hpp"
#include "core/base_path.hpp"
#include "core/clock.hpp"
#include "core/commands.hpp"
#include "core/input.hpp"
#include "core/log.hpp"
#include "core/project_manager.hpp"
#include "core/window.hpp"
#include "animation/animation_clip.hpp"
#include "animation/animator_component.hpp"
#include "animation/skeleton.hpp"
#include "renderer/components/arrow_component.hpp"
#include "renderer/resources/asset_loader.hpp"
#include "renderer/bindless.hpp"
#include "renderer/components/mesh_component.hpp"
#include "renderer/resources/model.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/components/skeletal_mesh_component.hpp"
#include "renderer/resources/skeletal_model.hpp"
#include "renderer/components/sprite_desc.hpp"
#include "renderer/resources/texture.hpp"
#include "renderer/vulkan_context.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/post_process_resources.hpp"
#include "scene/camera_node.hpp"
#include "scene/preset.hpp"
#include "scene/light_node.hpp"
#include "scripting/script_subsystem.hpp"
#include "ui/game_ui/game_ui.hpp"
#include "ui/icon_cache.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>
#include <GLFW/glfw3.h>

#include <glm/gtc/type_ptr.hpp>

#include <limits>

#include <filesystem>

namespace fjell {

SceneEditorContext::SceneEditorContext(SceneEditorDeps deps)
    : deps_{deps} {}

void SceneEditorContext::init() {
    viewport_manager_.init(deps_.vulkan_context->device_ref(),
                           deps_.vulkan_context->allocator(),
                           deps_.vulkan_context->msaa_samples());
    viewport_manager_.add_viewport(ViewportSource::final_color, "Viewport");

    hierarchy_panel_.init(deps_.scene, &viewport_manager_, &selected_id_,
                          &command_history(), deps_.icon_cache);
    properties_panel_.init(deps_.scene, &selected_id_, &gizmo_op_, deps_.light,
                           &deps_.script_subsystem->manager(), &command_history(),
                           &deps_.vulkan_context->grid_pass().settings(),
                           deps_.vulkan_context->pipeline_cache(),
                           &deps_.vulkan_context->bindless_textures(),
                           deps_.resource_manager, deps_.project_mgr,
                           deps_.content_browser, deps_.file_browser,
                           deps_.icon_cache);
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
    connections_.push_back(hierarchy_panel_.on_instantiate_preset.bind(
        [this](const std::string& rel_path, int32_t parent_id) {
            const auto* project = deps_.project_mgr ? deps_.project_mgr->active() : nullptr;
            auto resolved = base_path::resolve_asset(rel_path, project);
            auto result = Preset::load(resolved, *deps_.resource_manager, project);
            if (!result) {
                FJELL_CORE_ERROR("Failed to load preset: {}", result.error());
                return;
            }
            auto root_id = result->instantiate(*deps_.scene, parent_id);
            if (auto* root = deps_.scene->find(root_id)) {
                root->preset_source = rel_path;
            }
            selected_id_ = root_id;
        }));

    init_shared_panels(deps_.vulkan_context);
    history_panel().init(&command_history());
}

void SceneEditorContext::shutdown() {
    // Clean up preview resources (safe — called after GPU idle)
    camera_preview_pp_.reset();
    camera_preview_target_.reset();
    camera_preview_node_id_ = 0;

    node_editor_panel_.shutdown();
    viewport_manager_.release_textures();
    connections_.clear();
}

void SceneEditorContext::draw(float dt) {
    viewport_manager_.draw(*deps_.vulkan_context, *deps_.input, dt);

    // Camera preview PIP overlay (only after preview has rendered at least once)
    if (camera_preview_skip_frames_ < 0 && camera_preview_pp_ && camera_preview_pp_->tonemap()) {
        auto* vp = viewport_manager_.active_panel();
        if (vp && vp->img_size.x > 0 && vp->img_size.y > 0) {
            auto tex = camera_preview_pp_->tonemap()->imgui_texture(
                deps_.vulkan_context->tonemap_pass().output_sampler());
            if (tex) {
                bool visible = ImGui::Begin(vp->title.c_str());
                if (visible) {
                    float preview_w = 320.0f;
                    float preview_h = 180.0f;
                    float pad = 8.0f;
                    float border = 2.0f;

                    // Scale down if viewport is small
                    float max_w = vp->img_size.x * 0.35f;
                    if (preview_w > max_w) {
                        float scale = max_w / preview_w;
                        preview_w *= scale;
                        preview_h *= scale;
                    }

                    float x = vp->img_pos.x + vp->img_size.x - preview_w - pad;
                    float y = vp->img_pos.y + vp->img_size.y - preview_h - pad;

                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    dl->AddRectFilled(
                        {x - border, y - border},
                        {x + preview_w + border, y + preview_h + border},
                        IM_COL32(30, 30, 30, 220), 4.0f);
                    dl->AddRect(
                        {x - border, y - border},
                        {x + preview_w + border, y + preview_h + border},
                        IM_COL32(80, 80, 80, 200), 4.0f);
                    dl->AddImage(tex, {x, y}, {x + preview_w, y + preview_h});

                    // Label
                    auto* cam_node = dynamic_cast<CameraNode*>(
                        deps_.scene->find(selected_id_));
                    if (cam_node) {
                        const char* label = cam_node->name.c_str();
                        dl->AddText({x + 4.0f, y + 2.0f},
                                    IM_COL32(200, 200, 200, 200), label);
                    }
                }
                ImGui::End();
            }
        }
    }

    // Picking and gizmo on the active viewport
    auto* vp = viewport_manager_.active_panel();
    if (vp && vp->hovered) {
        draw_picking(*vp);
    }
    if (vp) {
        draw_gizmo(*vp);
    }

    // Play toolbar (centered in viewport toolbar area)
    draw_play_toolbar();

    hierarchy_panel_.draw();
    properties_panel_.draw();
    if (node_editor_panel_.visible()) {
        node_editor_panel_.draw();
    }

    if (focus_viewport_frames_ > 0) {
        if (--focus_viewport_frames_ == 0) {
            ImGui::SetWindowFocus("Viewport");
        }
    }

    draw_shared_panels(dt);
}

void SceneEditorContext::draw_picking(ViewportPanel& vp) {
    if (!ImGuizmo::IsUsing() && !ImGuizmo::IsOver() &&
        (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
         ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))) {
        auto mouse = ImGui::GetMousePos();
        float mx = (mouse.x - vp.img_pos.x) / vp.img_size.x;
        float my = (mouse.y - vp.img_pos.y) / vp.img_size.y;

        float aspect = vp.img_size.x / std::max(vp.img_size.y, 1.0f);
        auto view = vp.camera.view_matrix();
        auto proj = vp.camera.projection_matrix(aspect);
        auto inv_vp = glm::inverse(proj * view);

        glm::vec4 ndc_near{mx * 2.0f - 1.0f, my * 2.0f - 1.0f, 0.0f, 1.0f};
        glm::vec4 ndc_far {mx * 2.0f - 1.0f, my * 2.0f - 1.0f, 1.0f, 1.0f};

        glm::vec4 world_near = inv_vp * ndc_near;
        glm::vec4 world_far  = inv_vp * ndc_far;
        world_near /= world_near.w;
        world_far  /= world_far.w;

        glm::vec3 ray_origin = glm::vec3(world_near);
        glm::vec3 ray_dir = glm::normalize(glm::vec3(world_far - world_near));

        float closest_t = std::numeric_limits<float>::max();
        uint32_t hit_id = 0;

        for (const auto& obj : deps_.scene->objects()) {
            auto* mesh = obj->components.get<MeshComponent>();
            if (!mesh || !mesh->model) continue;
            if (!mesh->model->local_aabb().valid()) continue;
            const auto* spatial = obj->as_spatial();
            if (!spatial) continue;

            AABB world_aabb = mesh->model->local_aabb().transform(spatial->world_matrix_);
            glm::vec3 aabb_min = world_aabb.min;
            glm::vec3 aabb_max = world_aabb.max;

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
                    hit_id = obj->id;
                }
            }
        }

        if (hit_id != 0) {
            selected_id_ = hit_id;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (auto* hit_obj = deps_.scene->find_spatial(hit_id)) {
                    glm::vec3 center = glm::vec3(hit_obj->world_matrix_[3]);
                    float radius = 1.0f;
                    auto* hit_mesh = hit_obj->components.get<MeshComponent>();
                    if (hit_mesh && hit_mesh->model &&
                        !hit_mesh->model->collision_vertices().empty()) {
                        float max_r2 = 0.0f;
                        for (const auto& v : hit_mesh->model->collision_vertices()) {
                            auto wv = glm::vec3(hit_obj->world_matrix_ * glm::vec4(v, 1.0f));
                            max_r2 = std::max(max_r2, glm::dot(wv - center, wv - center));
                        }
                        radius = std::max(std::sqrt(max_r2), 0.1f);
                    }
                    vp.camera.focus(center, radius);
                }
            }
        } else {
            selected_id_ = 0;
        }
    }
}

void SceneEditorContext::draw_gizmo(ViewportPanel& vp) {
    auto* obj = deps_.scene->find_spatial(selected_id_);
    if (!obj) {
        gizmo_dragging_ = false;
        return;
    }

    // Must draw inside the viewport window for ImGuizmo
    if (!ImGui::Begin(vp.title.c_str())) {
        ImGui::End();
        gizmo_dragging_ = false;
        return;
    }

    float aspect = vp.img_size.x / std::max(vp.img_size.y, 1.0f);
    auto view = vp.camera.view_matrix();
    auto projection = vp.camera.projection_matrix(aspect);
    projection[1][1] *= -1.0f; // undo Vulkan Y-flip for ImGuizmo

    ImGuizmo::SetRect(vp.img_pos.x, vp.img_pos.y, vp.img_size.x, vp.img_size.y);
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist();

    // Snapshot at drag start
    if (!gizmo_dragging_) {
        gizmo_matrix_ = obj->world_matrix_;
        gizmo_start_world_ = obj->world_matrix_;
        gizmo_start_transform_ = obj->transform;
    }

    ImGuizmo::OPERATION op = ImGuizmo::TRANSLATE;
    if (gizmo_op_ == 1) op = ImGuizmo::ROTATE;
    if (gizmo_op_ == 2) op = ImGuizmo::SCALE;

    ImGuizmo::Manipulate(
        glm::value_ptr(view),
        glm::value_ptr(projection),
        op,
        ImGuizmo::LOCAL,
        glm::value_ptr(gizmo_matrix_));

    ImGui::End();

    auto apply_gizmo = [&] {
        glm::mat4 parent_world{1.0f};
        if (obj->parent_id != -1) {
            const auto* parent = deps_.scene->find(
                static_cast<uint32_t>(obj->parent_id));
            const auto* parent_spatial = parent ? parent->as_spatial() : nullptr;
            if (parent_spatial) parent_world = parent_spatial->world_matrix_;
        }
        glm::mat4 local_matrix = glm::inverse(parent_world) * gizmo_matrix_;
        obj->transform = decompose_matrix(local_matrix);
        obj->world_matrix_ = gizmo_matrix_;
    };

    if (ImGuizmo::IsUsing()) {
        gizmo_dragging_ = true;
        apply_gizmo();
        deps_.scene->mark_dirty();
    } else if (gizmo_dragging_) {
        gizmo_dragging_ = false;
        apply_gizmo();
        command_history().execute(std::make_unique<TransformCommand>(
            *deps_.scene, obj->id,
            gizmo_start_transform_, obj->transform,
            obj->name));
    }
}

void SceneEditorContext::draw_play_toolbar() {
    auto* vp = viewport_manager_.active_panel();
    if (!vp) return;

    auto* icons = IconCache::instance();
    if (!icons) return;

    // Must draw inside the viewport window's Begin/End
    if (!ImGui::Begin(vp->title.c_str())) {
        ImGui::End();
        return;
    }

    float icon_size = ImGui::GetFrameHeight() + 4.0f;

    float pad = 6.0f;
    float box_w = icon_size + pad * 2.0f;
    float box_h = icon_size + pad * 2.0f;
    float box_x = vp->img_pos.x + (vp->img_size.x - box_w) * 0.5f;
    float box_y = vp->img_pos.y + pad;

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(
        {box_x, box_y}, {box_x + box_w, box_y + box_h},
        IM_COL32(0, 0, 0, 128), 6.0f);

    // Invisible button for hit testing
    ImGui::SetCursorScreenPos({box_x + pad, box_y + pad});
    bool hovered = false;
    bool clicked = ImGui::InvisibleButton("##play_btn", {icon_size, icon_size});
    hovered = ImGui::IsItemHovered();

    // Draw icon with color change on hover
    const char* icon_name = is_playing() ? "stop" : "play";
    ImVec4 tint = is_playing()
        ? ImVec4{1.0f, 0.4f, 0.4f, 1.0f}
        : ImVec4{0.4f, 1.0f, 0.5f, 1.0f};
    if (hovered) {
        tint = is_playing()
            ? ImVec4{1.0f, 0.6f, 0.6f, 1.0f}
            : ImVec4{0.6f, 1.0f, 0.7f, 1.0f};
    }

    auto desc = icons->icon(icon_name);
    if (desc) {
        draw_list->AddImage(reinterpret_cast<ImTextureID>(desc),
            {box_x + pad, box_y + pad},
            {box_x + pad + icon_size, box_y + pad + icon_size},
            {0, 0}, {1, 1}, ImGui::ColorConvertFloat4ToU32(tint));
    }

    if (clicked) {
        if (is_playing()) {
            request_stop();
        } else {
            request_play();
        }
    }

    // Perspective / Orthographic toggle (top-right of viewport)
    {
        const char* label = vp->camera.mode() == EditorCameraMode::perspective
                            ? "Persp" : "Ortho";
        ImVec2 label_size = ImGui::CalcTextSize(label);
        float btn_w = label_size.x + 12.0f;
        float btn_h = ImGui::GetFrameHeight();
        float btn_x = vp->img_pos.x + vp->img_size.x - btn_w - pad;
        float btn_y = vp->img_pos.y + pad;

        ImGui::SetCursorScreenPos({btn_x, btn_y});
        if (ImGui::Button(label, {btn_w, btn_h})) {
            auto new_mode = vp->camera.mode() == EditorCameraMode::perspective
                            ? EditorCameraMode::orthographic
                            : EditorCameraMode::perspective;
            vp->camera.set_mode(new_mode);
        }
    }

    ImGui::End();
}

std::vector<std::string> SceneEditorContext::docked_window_names() const {
    return {"Hierarchy", ctx_title("History"), "Viewport",
            "Properties", ctx_title("Stats"), ctx_title("Console")};
}

void SceneEditorContext::setup_dockspace(ImGuiID main_area) {
    enum class EPanel { Hierarchy, History, Viewport, Properties, Stats, Console };
    using N = DockNode;
    static const auto layout = N::horizontal_split(0.14f,
        N::vertical_split(0.50f,
            N::leaf({EPanel::Hierarchy}),
            N::leaf({EPanel::History})),
        N::horizontal_split(0.78f,
            N::vertical_split(0.68f,
                N::leaf({EPanel::Viewport}),
                N::leaf({EPanel::Stats, EPanel::Console})),
            N::leaf({EPanel::Properties})));
    build_dockspace(main_area, layout, docked_window_names());
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

void SceneEditorContext::on_play_enter() {
    command_history().clear();
    play_session_ = std::make_unique<PlaySession>(*deps_.scene);
    selected_id_ = 0;
}

void SceneEditorContext::on_play_exit() {
    play_session_->restore(*deps_.scene);
    play_session_.reset();
    focus_viewport_frames_ = 2;
}

void SceneEditorContext::request_play() {
    deps_.start_play();
}

void SceneEditorContext::request_stop() {
    deps_.stop_play();
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
        request_stop();
    }
    ImGui::End();

    // FPS overlay: top-right corner, updates every 0.5s. Kept as an ImGui
    // draw instead of a {fps} game-UI binding so the player-facing HUD
    // doesn't rebuild its layout twice per second just to show a dev stat.
    {
        double dt = ImGui::GetIO().DeltaTime;
        play_fps_accum_s_ += dt;
        ++play_fps_count_;
        if (play_fps_accum_s_ >= 0.5) {
            play_fps_value_ = static_cast<float>(
                static_cast<double>(play_fps_count_) / play_fps_accum_s_);
            play_fps_accum_s_ = 0.0;
            play_fps_count_ = 0;
        }

        float fps_w = 96.0f;
        float fps_h = 28.0f;
        float fps_x = viewport->WorkPos.x + viewport->WorkSize.x - fps_w - 8.0f;
        float fps_y = viewport->WorkPos.y + 8.0f;
        ImGui::SetNextWindowPos({fps_x, fps_y});
        ImGui::SetNextWindowSize({fps_w, fps_h});
        ImGui::SetNextWindowBgAlpha(0.6f);
        ImGui::Begin("##PlayFps", nullptr,
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoDocking |
                     ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoInputs);
        ImGui::Text("FPS: %.0f", play_fps_value_);
        ImGui::End();
    }

    ImGui::End(); // ##PlayView
}

// --- Viewport ---

std::vector<ViewportRenderRequest> SceneEditorContext::build_render_requests() {
    // Preview is rendered separately via render_previews() — not included here
    return viewport_manager_.build_render_requests();
}

void SceneEditorContext::render_previews(VulkanContext& vk, Scene* scene, float dt,
                                          const DirectionalLight& light, ThreadPool& pool) {
    if (camera_preview_skip_frames_ > 0) {
        --camera_preview_skip_frames_;
        return;
    }
    if (!camera_preview_target_ || !camera_preview_pp_ || selected_id_ == 0) return;

    auto* cam = dynamic_cast<CameraNode*>(scene->find(selected_id_));
    if (!cam) return;

    ViewportRenderRequest req;
    req.target = camera_preview_target_.get();
    req.view = cam->view_matrix();
    req.projection = cam->projection_matrix(320.0f / 180.0f);
    req.camera_position = cam->position();
    req.pp_resources = camera_preview_pp_.get();
    req.skip_shadows = true;

    vk.render_preview(req, light, scene, dt, selected_id_, pool);

    // Signal that at least one render completed — overlay reads < 0
    camera_preview_skip_frames_ = -1;
}

void SceneEditorContext::apply_resize(VulkanContext& vk) {
    viewport_manager_.apply_resize(vk);

    // Camera preview lifecycle — safe here because begin_frame fence already waited
    bool need_preview = (selected_id_ != 0 && !is_playing() && deps_.scene);
    if (need_preview) {
        auto* node = deps_.scene->find(selected_id_);
        need_preview = (dynamic_cast<CameraNode*>(node) != nullptr);
    }

    if (need_preview && !camera_preview_target_) {
        // Use the same MSAA as the main viewport — pipelines are compiled
        // with this sample count and can't be changed per-viewport.
        camera_preview_target_ = std::make_unique<OffscreenPass>(
            vk.device_ref(), vk.allocator(), 320, 180, vk.msaa_samples());

        PostProcessConfig preview_config{};
        preview_config.hiz = false;
        preview_config.gtao = false;
        preview_config.bloom = false;
        preview_config.ssr = false;
        preview_config.volumetric_fog = false;
        preview_config.tonemap = true;

        PostProcessResources::PassRefs refs{
            vk.hiz_pass(), vk.gtao_pass(), vk.bloom_pass(),
            vk.ssr_pass(), vk.volumetric_fog_pass(), vk.tonemap_pass(), vk.ddgi_pass(), vk.ss_gi_pass(), vk.taa_resolve_pass()};

        camera_preview_pp_ = std::make_unique<PostProcessResources>(
            vk.device(), vk.allocator(), 320, 180, preview_config, refs);
        camera_preview_node_id_ = selected_id_;
        // Skip rendering for MAX_FRAMES_IN_FLIGHT frames to let all in-flight
        // command buffers drain before the preview's descriptors are first used
        camera_preview_skip_frames_ = 3;
    } else if (!need_preview && camera_preview_target_) {
        // Must wait for all in-flight command buffers that reference
        // the preview's descriptor sets / images before destroying them.
        vk.wait_idle();
        camera_preview_pp_.reset();
        camera_preview_target_.reset();
        camera_preview_node_id_ = 0;
        camera_preview_skip_frames_ = 0;
    }
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

    // SkeletalMeshComponent — rendering only (model + material)
    auto& smc = node->components.add<SkeletalMeshComponent>();
    smc.skeletal_model_handle = skel_model_handle;
    smc.skeletal_model = skel_model;

    // Default material
    auto tex = deps_.resource_manager->load_texture(
        base_path::resolve("engine_assets/textures/test.png")).value_or(TextureHandle{});
    smc.material_handle = deps_.resource_manager->create_material("skel_default", tex);
    smc.material = deps_.resource_manager->get(smc.material_handle);

    // AnimatorComponent — animation data (skeleton comes from SkeletalModel)
    auto& anim = node->components.add<AnimatorComponent>();
    if (smc.skeletal_model && smc.skeletal_model->skeleton()) {
        anim.init(smc.skeletal_model->skeleton());
    }

    // Play animation clip if provided
    if (!anim_path.empty()) {
        auto resolved_anim = base_path::resolve_asset(anim_path, deps_.project_mgr->active());
        auto clip_result = AnimationClip::load(resolved_anim);
        if (clip_result) {
            auto clip = std::make_shared<AnimationClip>(std::move(*clip_result));
            anim.play(clip.get());
            anim.clips.push_back(clip);
            anim.clip_paths.push_back(anim_path);
            FJELL_CORE_INFO("Playing animation: {}", anim_path);
        } else {
            FJELL_CORE_WARN("Failed to load animation {}: {}", anim_path, clip_result.error());
        }
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
