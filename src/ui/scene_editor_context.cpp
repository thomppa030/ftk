#include "ui/scene_editor_context.hpp"
#include "ui/dockspace_layout.hpp"
#include "ui/icons_lc.hpp"
#include "core/base_path.hpp"
#include "core/clock.hpp"
#include "core/commands.hpp"
#include "core/input.hpp"
#include "core/log.hpp"
#include "core/play_controller.hpp"
#include "core/project_manager.hpp"
#include "core/window.hpp"
#include "animation/animation_clip.hpp"
#include "animation/animator_component.hpp"
#include "animation/skeleton.hpp"
#include "renderer/components/arrow_component.hpp"
#include "renderer/resources/asset_loader.hpp"
#include "renderer/bindless.hpp"
#include "physics/physics_types.hpp"
#include "renderer/components/mesh_component.hpp"
#include "renderer/resources/model.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/components/skeletal_mesh_component.hpp"
#include "renderer/resources/skeletal_model.hpp"
#include "renderer/components/spline_component.hpp"
#include "renderer/components/sprite_desc.hpp"
#include "scene/spline_editable.hpp"
#include "scripting/debug_draw_queue.hpp"
#include "scripting/screen_debug_queue.hpp"
#include "renderer/resources/texture.hpp"
#include "renderer/vulkan_context.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/post_process_resources.hpp"
#include "scene/camera_node.hpp"
#include "scene/camera_subsystem.hpp"
#include "scene/preset.hpp"
#include "scene/light_node.hpp"
#include "scripting/script_subsystem.hpp"
#include "net/net_manager.hpp"
#include "ui/game_ui/game_ui.hpp"
#include "ui/icon_cache.hpp"

#include <algorithm>
#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>
#include <GLFW/glfw3.h>

#include <glm/gtc/type_ptr.hpp>

#include <cstdio>
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
    properties_panel_.init(deps_.scene, &selected_id_, deps_.light,
                           &deps_.script_subsystem->manager(), &command_history(),
                           &deps_.vulkan_context->grid_pass().settings(),
                           deps_.vulkan_context->pipeline_cache(),
                           &deps_.vulkan_context->bindless_textures(),
                           deps_.resource_manager, deps_.project_mgr,
                           deps_.content_browser, deps_.file_browser,
                           deps_.icon_cache);
    node_editor_panel_.init(deps_.scene, &selected_id_);

    // Undo commands mutate a scene that stop() throws away, and a selection
    // made in the editor rarely means anything once the game rearranges it.
    connections_.push_back(deps_.play->on_started.bind([this] {
        command_history().clear();
        selected_id_ = 0;
    }));
    connections_.push_back(deps_.play->on_stopped.bind([this] {
        focus_viewport_frames_ = 2;
    }));

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

    init_shared_panels(deps_.vulkan_context, deps_.project_mgr);
    history_panel().init(&command_history());
    net_panel_.init(deps_.net_manager);
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

    // Spline viewport overlays (polyline + control points for the selected node)
    draw_spline_visuals();

    // Script-submitted debug text, anchored to the active viewport image so
    // watches stay readable while scripts tick in the editor. Drawn inside the
    // viewport window so it clips there instead of over the editor panels.
    if (vp && vp->img_size.x > 0.0f && vp->img_size.y > 0.0f) {
        if (ImGui::Begin(vp->title.c_str())) {
            draw_screen_debug(ImGui::GetWindowDrawList(), vp->img_pos, vp->img_size);
        }
        ImGui::End();
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
    net_panel_.draw(dt, ctx_title("Network").c_str());
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

        // ── Spline control-point pick (any spline-bearing component) ──
        // Ray-test every control point on every node that carries a
        // spline (via SplineEditableRegistry). On a hit, select the owning
        // node and its handle in one click. Takes priority over mesh
        // AABB picking.
        {
            constexpr float kHandleRadius = 0.12f;
            auto& registry = SplineEditableRegistry::instance();
            float    best_t  = std::numeric_limits<float>::max();
            uint32_t best_node{0};
            int      best_point{-1};

            for (const auto& obj : deps_.scene->objects()) {
                const auto* spatial = obj->as_spatial();
                if (!spatial) continue;
                const auto editable = registry.probe(*obj);
                if (!editable.spline) continue;

                const glm::mat4& world = spatial->world_matrix_;
                const auto& pts = editable.spline->points;
                for (size_t i = 0; i < pts.size(); ++i) {
                    const glm::vec3 center = glm::vec3(
                        world * glm::vec4(pts[i].value, 1.0f));
                    const glm::vec3 oc = ray_origin - center;
                    const float b = glm::dot(oc, ray_dir);
                    const float c = glm::dot(oc, oc) - kHandleRadius * kHandleRadius;
                    const float disc = b * b - c;
                    if (disc < 0.0f) continue;
                    const float sqrt_d = std::sqrt(disc);
                    const float t0 = -b - sqrt_d;
                    const float t = t0 >= 0.0f ? t0 : (-b + sqrt_d);
                    if (t >= 0.0f && t < best_t) {
                        best_t     = t;
                        best_node  = obj->id;
                        best_point = static_cast<int>(i);
                    }
                }
            }

            if (best_point >= 0) {
                // Clear handle selection on every other spline so only one
                // is ever "active" across the whole scene.
                for (const auto& obj : deps_.scene->objects()) {
                    const auto other = registry.probe(*obj);
                    if (other.selected_point && obj->id != best_node) {
                        *other.selected_point = -1;
                    }
                }
                selected_id_ = best_node;
                if (auto* hit_obj = deps_.scene->find_spatial(best_node)) {
                    const auto hit = registry.probe(*hit_obj);
                    if (hit.selected_point) *hit.selected_point = best_point;
                }
                return;  // don't fall through to node picking
            }
        }

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

    // If the selected object carries any spline with a selected control
    // point, the gizmo manipulates that point instead of the node.
    const auto editable = SplineEditableRegistry::instance().probe(*obj);
    const int  sel_idx  = (editable.selected_point != nullptr) ? *editable.selected_point : -1;
    const bool spline_point_mode = editable.spline
        && sel_idx >= 0
        && sel_idx < static_cast<int>(editable.spline->points.size());

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
        if (spline_point_mode) {
            const glm::vec3 local_pos = editable.spline->points[sel_idx].value;
            gizmo_matrix_ = obj->world_matrix_ * glm::translate(glm::mat4(1.0f), local_pos);
            gizmo_start_world_ = gizmo_matrix_;
            gizmo_start_transform_ = obj->transform;
        } else {
            gizmo_matrix_ = obj->world_matrix_;
            gizmo_start_world_ = obj->world_matrix_;
            gizmo_start_transform_ = obj->transform;
        }
    }

    // Spline points only translate (rotation/scale would rotate the node, not the point).
    ImGuizmo::OPERATION op = ImGuizmo::TRANSLATE;
    if (!spline_point_mode) {
        if (gizmo_op_ == 1) op = ImGuizmo::ROTATE;
        if (gizmo_op_ == 2) op = ImGuizmo::SCALE;
    }

    // Snapping. Ctrl inverts the toolbar toggle, so a coarse grid can be
    // escaped (or a fine one temporarily enforced) without leaving the drag.
    // ImGuizmo reads as many components as the operation uses: three for
    // translate and scale, one for rotate.
    const float* snap = nullptr;
    glm::vec3 snap_values{0.0f};
    if (deps_.vulkan_context) {
        const auto& grid = deps_.vulkan_context->grid_pass().settings();
        if (grid.snap_enabled != ImGui::GetIO().KeyCtrl) {
            switch (op) {
                case ImGuizmo::ROTATE:
                    snap_values = glm::vec3(grid.rotation_snap_degrees);
                    break;
                case ImGuizmo::SCALE:
                    snap_values = glm::vec3(grid.scale_snap);
                    break;
                default:
                    snap_values = glm::vec3(grid.cell_size);
                    break;
            }
            snap = glm::value_ptr(snap_values);
        }
    }

    ImGuizmo::Manipulate(
        glm::value_ptr(view),
        glm::value_ptr(projection),
        op,
        ImGuizmo::LOCAL,
        glm::value_ptr(gizmo_matrix_),
        nullptr,
        snap);

    ImGui::End();

    auto apply_node_gizmo = [&] {
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

    auto apply_spline_gizmo = [&] {
        // Extract world-space position from the gizmo matrix, then map back
        // into the node's local space, then delegate post-edit side effects
        // (auto tangents, reparam rebuild, dirty flag) to the component.
        const glm::vec3 world_pos = glm::vec3(gizmo_matrix_[3]);
        const glm::vec3 local_pos = glm::vec3(
            glm::inverse(obj->world_matrix_) * glm::vec4(world_pos, 1.0f));
        editable.spline->points[sel_idx].value = local_pos;
        if (editable.on_edited) editable.on_edited();
    };

    if (ImGuizmo::IsUsing()) {
        gizmo_dragging_ = true;
        if (spline_point_mode) apply_spline_gizmo();
        else                   apply_node_gizmo();
        deps_.scene->mark_dirty();
    } else if (gizmo_dragging_) {
        gizmo_dragging_ = false;
        if (spline_point_mode) {
            apply_spline_gizmo();
            // Undo support for spline edits: skip for now — same reason as
            // instanced-mesh per-instance drags (no command type yet).
        } else {
            apply_node_gizmo();
            command_history().execute(std::make_unique<TransformCommand>(
                *deps_.scene, obj->id,
                gizmo_start_transform_, obj->transform,
                obj->name));
        }
    }
}

void SceneEditorContext::draw_spline_visuals() {
    auto* queue = deps_.vulkan_context->debug_draw_queue();
    if (!queue) return;

    auto* obj = deps_.scene->find_spatial(selected_id_);
    if (!obj) return;

    const auto editable = SplineEditableRegistry::instance().probe(*obj);
    if (!editable.spline || editable.spline->points.empty()) return;
    const auto& spline = *editable.spline;
    const int selected_idx = editable.selected_point ? *editable.selected_point : -1;

    const glm::mat4& world = obj->world_matrix_;

    auto to_world = [&](const glm::vec3& local) {
        return glm::vec3(world * glm::vec4(local, 1.0f));
    };

    // Polyline: sample N segments using the reparam table when populated,
    // otherwise fall back to direct t-parameter sampling.
    constexpr int kSamples = 64;
    const glm::vec4 curve_col{0.45f, 0.75f, 0.95f, 1.0f};

    if (spline.points.size() >= 2) {
        const float len = spline.total_length();
        glm::vec3 prev = to_world(spline.evaluate(spline.min_key()));
        if (len > 0.0f) {
            for (int i = 1; i <= kSamples; ++i) {
                const float s = len * (static_cast<float>(i) / static_cast<float>(kSamples));
                const glm::vec3 w = to_world(spline.evaluate_at_distance(s));
                queue->line(prev, w, curve_col);
                prev = w;
            }
        } else {
            const float k0 = spline.min_key();
            const float k1 = spline.max_key();
            for (int i = 1; i <= kSamples; ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(kSamples);
                const glm::vec3 w = to_world(spline.evaluate(k0 + (k1 - k0) * t));
                queue->line(prev, w, curve_col);
                prev = w;
            }
        }
    }

    // Control-point handles: sphere per point, selected one highlighted.
    constexpr float kHandleRadius = 0.08f;
    const glm::vec4 pt_col{1.0f, 1.0f, 1.0f, 1.0f};
    const glm::vec4 pt_sel{1.0f, 0.65f, 0.15f, 1.0f};
    for (size_t i = 0; i < spline.points.size(); ++i) {
        const glm::vec3 w = to_world(spline.points[i].value);
        const bool selected = (selected_idx == static_cast<int>(i));
        queue->sphere(w, kHandleRadius, selected ? pt_sel : pt_col);
    }

    // Tangent handles for CurveUser / CurveBreak: draw a line from the point
    // to a ball at position + tangent * (segment span * 0.25) on each side.
    const glm::vec4 tan_col{0.6f, 1.0f, 0.5f, 1.0f};
    const auto& pts = spline.points;
    for (size_t i = 0; i < pts.size(); ++i) {
        const auto& p = pts[i];
        if (p.interp_mode != SplineInterp::CurveUser &&
            p.interp_mode != SplineInterp::CurveBreak) continue;

        // Tangents are dValue/dInputKey — scale into "handle length" using the
        // adjacent key spans so the visuals look similar regardless of key scale.
        const float dx_prev = (i > 0)             ? (p.input_key - pts[i - 1].input_key) : 1.0F;
        const float dx_next = (i + 1 < pts.size()) ? (pts[i + 1].input_key - p.input_key) : 1.0F;
        const glm::vec3 pos = p.value;
        const glm::vec3 handle_in  = pos - p.arrive_tangent * (dx_prev * 0.25F);
        const glm::vec3 handle_out = pos + p.leave_tangent  * (dx_next * 0.25F);

        const glm::vec3 pw = to_world(pos);
        const glm::vec3 hin_w  = to_world(handle_in);
        const glm::vec3 hout_w = to_world(handle_out);
        queue->line(pw, hin_w, tan_col);
        queue->line(pw, hout_w, tan_col);
        queue->sphere(hin_w,  kHandleRadius * 0.7F, tan_col);
        queue->sphere(hout_w, kHandleRadius * 0.7F, tan_col);
    }
}

namespace {

// One toolbar toggle: an icon that reads as pressed while `on` is true.
// Returns true on click. `tooltip` carries the name and shortcut, since the
// buttons are icon-only.
bool toolbar_toggle(const char* id, const char* glyph, bool on, const char* tooltip) {
    ImVec4 active{0.26f, 0.59f, 0.98f, 0.65f};
    ImVec4 idle{0.0f, 0.0f, 0.0f, 0.0f};
    ImGui::PushStyleColor(ImGuiCol_Button, on ? active : idle);
    ImGui::PushID(id);
    bool clicked = ImGui::Button(glyph);
    ImGui::PopID();
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tooltip);
    }
    return clicked;
}

} // namespace

SceneEditorContext::PlayControlsRect SceneEditorContext::draw_play_controls(
    ImDrawList* dl, const glm::vec2& origin, const glm::vec2& size) {
    auto* icons = IconCache::instance();
    if (!icons) return {};

    float icon_size = ImGui::GetFrameHeight() + 4.0f;
    float pad = 6.0f;
    float inner_spacing = 4.0f;

    const bool playing = deps_.play->playing();
    const bool paused = deps_.play->paused();

    // Play/stop always shows; pause, step and the time-scale row only make
    // sense once playing — showing them disabled while stopped would invite
    // clicking a control that can't do anything yet.
    const int button_count = playing ? 3 : 1;
    const float scale_row_h = playing ? ImGui::GetFrameHeight() + pad : 0.0f;
    float box_w = pad * 2.0f + static_cast<float>(button_count) * icon_size
                + static_cast<float>(button_count - 1) * inner_spacing;
    box_w = std::max(box_w, playing ? 140.0f : 0.0f); // room for the scale drag
    float box_h = icon_size + pad * 2.0f + scale_row_h;
    float box_x = origin.x + (size.x - box_w) * 0.5f;
    float box_y = origin.y + pad;

    dl->AddRectFilled(
        {box_x, box_y}, {box_x + box_w, box_y + box_h},
        IM_COL32(0, 0, 0, 128), 6.0f);

    // Icons are centered within the box: it can be wider than the icon
    // cluster once the time-scale row forces a minimum width.
    const float icons_w = static_cast<float>(button_count) * icon_size
                        + static_cast<float>(button_count - 1) * inner_spacing;
    const float icons_x0 = box_x + (box_w - icons_w) * 0.5f;

    // Bindless-texture icon button drawn at a fixed slot in the box, sharing
    // the invisible-button-then-AddImage pattern the play button established
    // (so a click still registers even though there's no ImGui::Button glyph
    // to hit-test against under the custom tint).
    auto icon_button = [&](int slot, const char* icon_name, bool active,
                           const ImVec4& idle_tint, const ImVec4& active_tint,
                           const char* tooltip) -> bool {
        float x = icons_x0 + static_cast<float>(slot) * (icon_size + inner_spacing);
        ImGui::SetCursorScreenPos({x, box_y + pad});
        ImGui::PushID(slot);
        bool clicked = ImGui::InvisibleButton("##toolbar_icon", {icon_size, icon_size});
        bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();

        ImVec4 tint = active ? active_tint : idle_tint;
        if (hovered) {
            tint.x = std::min(tint.x + 0.2f, 1.0f);
            tint.y = std::min(tint.y + 0.2f, 1.0f);
            tint.z = std::min(tint.z + 0.2f, 1.0f);
        }

        auto desc = icons->icon(icon_name);
        if (desc) {
            dl->AddImage(reinterpret_cast<ImTextureID>(desc),
                {x, box_y + pad}, {x + icon_size, box_y + pad + icon_size},
                {0, 0}, {1, 1}, ImGui::ColorConvertFloat4ToU32(tint));
        }
        if (hovered && tooltip) ImGui::SetTooltip("%s", tooltip);
        return clicked;
    };

    if (icon_button(0, playing ? "stop" : "play", playing,
                    {0.4f, 1.0f, 0.5f, 1.0f}, {1.0f, 0.4f, 0.4f, 1.0f},
                    playing ? "Stop (Ctrl+P)" : "Play (Ctrl+P)")) {
        deps_.play->toggle();
    }

    if (playing) {
        if (icon_button(1, "pause", paused,
                        {0.85f, 0.85f, 0.85f, 1.0f}, {1.0f, 0.85f, 0.3f, 1.0f},
                        paused ? "Resume (Ctrl+Shift+P)" : "Pause (Ctrl+Shift+P)")) {
            deps_.play->toggle_pause();
        }
        if (icon_button(2, "step", false,
                        {0.85f, 0.85f, 0.85f, 1.0f}, {0.85f, 0.85f, 0.85f, 1.0f},
                        "Step one frame (Ctrl+Alt+P)")) {
            deps_.play->step();
        }

        // Time-scale row: a narrow drag field centered under the icons.
        float scale = deps_.play->time_scale();
        float drag_w = box_w - pad * 2.0f;
        ImGui::SetCursorScreenPos({box_x + pad, box_y + icon_size + pad * 2.0f});
        ImGui::SetNextItemWidth(drag_w);
        ImGui::PushID("play_time_scale");
        if (ImGui::DragFloat("##scale", &scale, 0.01f, 0.1f, 4.0f, "%.2fx")) {
            deps_.play->set_time_scale(scale);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Time scale");
        }
        ImGui::PopID();
    }

    return {box_x, box_y, box_w, box_h};
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

    const bool playing = deps_.play->playing();
    const float pad = 6.0f;

    if (playing) {
        scene_image_rect_.pos = vp->img_pos;
        scene_image_rect_.size = vp->img_size;

        // HUD composited on top of the docked panel's own scene image,
        // which viewport_manager_.draw() already drew before this runs —
        // see draw_play_overlay for the fullscreen equivalent.
        auto hud_tex = deps_.play->hud_texture();
        if (hud_tex) {
            ImGui::SetCursorScreenPos({vp->img_pos.x, vp->img_pos.y});
            ImGui::Image(hud_tex, {vp->img_size.x, vp->img_size.y});
        }
    }

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    auto box = draw_play_controls(draw_list, vp->img_pos, vp->img_size);
    const float box_x = box.x, box_y = box.y, box_w = box.w, box_h = box.h;

    // Net mode selector — sits just left of the play button. The engine never
    // acts on this; it sets NetManager's launch mode, which the running game
    // script reads in begin_play to host/join/stay offline. Hidden while playing
    // (mode is latched for the session) and when there's no NetManager.
    // Room for the selector between the viewport's left edge and the play box?
    // If not it is skipped rather than sliding off or overlapping.
    const float kNetComboW = 78.0f;
    const bool net_selector_fits =
        (box_x - kNetComboW - pad) >= (vp->img_pos.x + pad);

    if (deps_.net_manager && !playing && net_selector_fits) {
        static const char* const kModeLabels[] = {"Offline", "Host", "Join"};
        auto mode = deps_.net_manager->launch_mode();
        int mode_idx = static_cast<int>(mode);

        const char* label = kModeLabels[mode_idx];
        float combo_w = kNetComboW;
        float combo_h = ImGui::GetFrameHeight();
        float combo_x = box_x - combo_w - pad;
        float combo_y = box_y + (box_h - combo_h) * 0.5f;

        ImGui::SetCursorScreenPos({combo_x, combo_y});
        ImGui::SetNextItemWidth(combo_w);
        if (ImGui::BeginCombo("##net_mode", label, ImGuiComboFlags_None)) {
            for (int i = 0; i < 3; ++i) {
                bool selected = (mode_idx == i);
                if (ImGui::Selectable(kModeLabels[i], selected)) {
                    deps_.net_manager->set_launch_mode(static_cast<NetLaunchMode>(i));
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        // Join address field, shown only in Join mode.
        if (mode == NetLaunchMode::join) {
            char addr_buf[64];
            const std::string& addr = deps_.net_manager->join_address();
            std::snprintf(addr_buf, sizeof(addr_buf), "%s", addr.c_str());

            ImGui::SetCursorScreenPos({combo_x, combo_y + combo_h + 4.0f});
            ImGui::SetNextItemWidth(combo_w);
            if (ImGui::InputText("##net_addr", addr_buf, sizeof(addr_buf))) {
                deps_.net_manager->set_join_address(addr_buf);
            }
        }
    }

    // Left cluster: gizmo mode, then the view toggles. Both act on the
    // viewport, so they live over it rather than in a panel or a menu.
    //
    // The play box is centered, so the space a left-aligned cluster can use is
    // the gap before it. Buttons are dropped from the right (lowest priority
    // first) rather than being allowed to slide under the play button.
    {
        const float spacing = 2.0f;
        const float btn_w = ImGui::CalcTextSize(ICON_LC_MOVE).x
                          + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float sep_w = ImGui::CalcTextSize("|").x + spacing;
        const float budget = (box_x - pad) - (vp->img_pos.x + pad);

        auto fits = [&](int buttons, bool separator) {
            float w = static_cast<float>(buttons) * (btn_w + spacing);
            if (separator) { w += sep_w; }
            return w <= budget;
        };

        const bool has_vk = deps_.vulkan_context != nullptr;
        // 3 gizmo + separator + 3 view toggles, shed in that order.
        const bool show_views = has_vk && fits(6, true);
        const bool show_gizmo = fits(3, false);

        if (show_gizmo) {
            ImGui::SetCursorScreenPos({vp->img_pos.x + pad, vp->img_pos.y + pad});
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {spacing, 0.0f});

            if (toolbar_toggle("gizmo_move", ICON_LC_MOVE, gizmo_op_ == 0, "Move (W)")) {
                gizmo_op_ = 0;
            }
            ImGui::SameLine();
            if (toolbar_toggle("gizmo_rotate", ICON_LC_ROTATE_3D, gizmo_op_ == 1, "Rotate (E)")) {
                gizmo_op_ = 1;
            }
            ImGui::SameLine();
            if (toolbar_toggle("gizmo_scale", ICON_LC_SCALE_3D, gizmo_op_ == 2, "Scale (R)")) {
                gizmo_op_ = 2;
            }

            if (show_views) {
                auto& grid = deps_.vulkan_context->grid_pass().settings();

                ImGui::SameLine();
                ImGui::TextDisabled("|");

                ImGui::SameLine();
                bool colliders = deps_.vulkan_context->show_colliders();
                if (toolbar_toggle("show_colliders", ICON_LC_BOX, colliders,
                                   "Show colliders")) {
                    deps_.vulkan_context->set_show_colliders(!colliders);
                }
                ImGui::SameLine();
                if (toolbar_toggle("show_grid", ICON_LC_GRID_3X3, grid.visible,
                                   "Show grid")) {
                    grid.visible = !grid.visible;
                }
                ImGui::SameLine();
                if (toolbar_toggle("snap_grid", ICON_LC_MAGNET, grid.snap_enabled,
                                   "Snap to grid")) {
                    grid.snap_enabled = !grid.snap_enabled;
                }
            }

            ImGui::PopStyleVar();
        }
    }

    // Perspective / Orthographic toggle (top-right of viewport)
    {
        bool persp = vp->camera.mode() == EditorCameraMode::perspective;
        const char* label = persp ? ICON_LC_CAMERA : ICON_LC_SQUARE;
        const char* tip = persp ? "Perspective — click for orthographic"
                                : "Orthographic — click for perspective";
        ImVec2 label_size = ImGui::CalcTextSize(label);
        float btn_w = label_size.x + 12.0f;
        float btn_h = ImGui::GetFrameHeight();
        float btn_x = vp->img_pos.x + vp->img_size.x - btn_w - pad;
        float btn_y = vp->img_pos.y + pad;

        // Only draw when it clears the play box.
        if (btn_x < box_x + box_w + pad) {
            ImGui::End();
            return;
        }

        ImGui::SetCursorScreenPos({btn_x, btn_y});
        if (ImGui::Button(label, {btn_w, btn_h})) {
            vp->camera.set_mode(persp ? EditorCameraMode::orthographic
                                      : EditorCameraMode::perspective);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tip);
        }
    }

    ImGui::End();
}

std::vector<std::string> SceneEditorContext::docked_window_names() const {
    return {"Hierarchy", ctx_title("History"), "Viewport",
            "Properties", ctx_title("Stats"), ctx_title("Console"),
            ctx_title("Network")};
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
    if (ImGui::IsAnyItemActive()) return false;

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

    // Drop the selection onto whatever is below it.
    //
    // ImGui's keyboard nav treats End as "focus the last item in the window",
    // which in the viewport overlay is the net mode selector. That runs inside
    // NewFrame(), before any of this, so claiming the key only on the press
    // would always be a frame too late. Taking ownership every frame means the
    // claim is already in place when nav polls End (it requires an unowned key),
    // and ownership has to be renewed because it is dropped again on any frame
    // where the key is not held.
    ImGui::SetKeyOwner(ImGuiKey_End, ImGui::GetID("##snap_to_floor"));

    if (ImGui::IsKeyPressed(ImGuiKey_End)) {
        snap_selection_to_floor();
        return true;
    }

    return false;
}

// --- Play mode ---

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

    scene_image_rect_.pos = {viewport->WorkPos.x, viewport->WorkPos.y};
    scene_image_rect_.size = {viewport->WorkSize.x, viewport->WorkSize.y};

    // Scene image fills the window — use active viewport's offscreen
    auto tex_id = viewport_manager_.active_texture(*deps_.vulkan_context);
    if (tex_id) {
        ImGui::Image(tex_id, viewport->WorkSize);
    }

    // HUD composited on top: a transparent texture PlayController rendered
    // separately (see PlayController::record_hud), not baked into the scene
    // image, so it can be resized and hit-tested independently of it.
    auto hud_tex = deps_.play->hud_texture();
    if (hud_tex) {
        ImGui::SetCursorScreenPos(viewport->WorkPos);
        ImGui::Image(hud_tex, viewport->WorkSize);
    }

    // Play/stop/pause/step/time-scale box, top center. Same control drawn
    // over the docked viewport panel while editing — see draw_play_toolbar.
    draw_play_controls(ImGui::GetWindowDrawList(),
                       {viewport->WorkPos.x, viewport->WorkPos.y},
                       {viewport->WorkSize.x, viewport->WorkSize.y});

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

    // Script-submitted debug text, over the whole play view. Still inside
    // ##PlayView's Begin/End, so this uses that window's draw list.
    draw_screen_debug(ImGui::GetWindowDrawList(),
                      {viewport->WorkPos.x, viewport->WorkPos.y},
                      {viewport->WorkSize.x, viewport->WorkSize.y});

    ImGui::End(); // ##PlayView

    // The network panel is a debug/testing tool whose whole point is to be used
    // *during* play — that's when conditioning sliders and live RTT/bandwidth
    // matter. The rest of the editor chrome stays hidden, but this one floats on
    // top of the play view. Only shown while networked, to avoid clutter in
    // single-player playtests.
    if (deps_.net_manager && deps_.net_manager->is_connected()) {
        net_panel_.draw(ImGui::GetIO().DeltaTime, ctx_title("Network").c_str());
    }
}

void SceneEditorContext::draw_screen_debug(ImDrawList* dl, const glm::vec2& origin,
                                           const glm::vec2& size) {
    if (!dl) return;
    auto* queue = deps_.screen_debug_queue;
    if (!queue || queue->entries().empty()) return;
    if (size.x <= 0.0f || size.y <= 0.0f) return;

    // Sort into the three blocks. Entries are pointers into the queue — it
    // isn't mutated until tick(), which runs after this.
    std::vector<const ScreenDebugEntry*> messages;
    std::vector<const ScreenDebugEntry*> watches;
    std::vector<const ScreenDebugEntry*> world;
    for (const auto& e : queue->entries()) {
        switch (e.kind) {
            case ScreenDebugEntry::Kind::Message: messages.push_back(&e); break;
            case ScreenDebugEntry::Kind::Watch:   watches.push_back(&e);  break;
            case ScreenDebugEntry::Kind::World:   world.push_back(&e);    break;
        }
    }

    // Messages keep submission order; watches sort by name so a row holds the
    // same spot every frame instead of moving as scripts submit in a new order.
    std::sort(messages.begin(), messages.end(),
              [](const ScreenDebugEntry* a, const ScreenDebugEntry* b) {
                  return a->seq < b->seq;
              });
    std::sort(watches.begin(), watches.end(),
              [](const ScreenDebugEntry* a, const ScreenDebugEntry* b) {
                  return a->name < b->name;
              });

    // A dark outline behind each glyph keeps text readable over a bright or
    // busy scene without needing a filled panel behind the whole block.
    auto draw_text_outlined = [&](const ImVec2& pos, const glm::vec4& color,
                                  const char* text) {
        ImU32 shadow = IM_COL32(0, 0, 0, static_cast<int>(color.a * 200.0f));
        dl->AddText({pos.x + 1.0f, pos.y + 1.0f}, shadow, text);
        dl->AddText(pos, ImGui::ColorConvertFloat4ToU32({color.r, color.g, color.b, color.a}), text);
    };

    const float line_h = ImGui::GetTextLineHeightWithSpacing();
    const float pad = 8.0f;

    // Watches: top-left, "name: value".
    float y = origin.y + pad;
    for (const auto* e : watches) {
        std::string line = e->name + ": " + e->text;
        draw_text_outlined({origin.x + pad, y}, e->color, line.c_str());
        y += line_h;
    }

    // Messages: below the watch block, same column.
    if (!watches.empty() && !messages.empty()) y += line_h * 0.5f;
    for (const auto* e : messages) {
        draw_text_outlined({origin.x + pad, y}, e->color, e->text.c_str());
        y += line_h;
    }

    // World-anchored text: project through the camera that actually rendered
    // this view — the scene CameraNode while playing (Engine overrides the
    // request with it), the editor fly-camera otherwise.
    if (world.empty()) return;

    glm::mat4 view{1.0f};
    glm::mat4 proj{1.0f};
    const float aspect = size.x / size.y;
    bool have_camera = false;

    if (deps_.play->playing() && deps_.scene && deps_.camera_subsystem) {
        if (auto* cam = deps_.camera_subsystem->active_camera(*deps_.scene)) {
            view = cam->view_matrix();
            proj = cam->projection_matrix(aspect);
            have_camera = true;
        }
    }
    if (!have_camera) {
        if (auto* cam = viewport_manager_.active_camera()) {
            view = cam->view_matrix();
            proj = cam->projection_matrix(aspect);
            have_camera = true;
        }
    }
    if (!have_camera) return;

    const glm::mat4 view_proj = proj * view;
    for (const auto* e : world) {
        glm::vec4 clip = view_proj * glm::vec4(e->world_pos, 1.0f);
        if (clip.w <= 0.0f) continue;  // behind the camera

        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        if (ndc.x < -1.0f || ndc.x > 1.0f || ndc.y < -1.0f || ndc.y > 1.0f) continue;

        // Projection already carries the Vulkan Y-flip, so NDC y maps straight
        // down the screen the same way x maps across it.
        ImVec2 pos{origin.x + (ndc.x * 0.5f + 0.5f) * size.x,
                   origin.y + (ndc.y * 0.5f + 0.5f) * size.y};

        ImVec2 text_size = ImGui::CalcTextSize(e->text.c_str());
        pos.x -= text_size.x * 0.5f;  // centre on the anchor point
        draw_text_outlined(pos, e->color, e->text.c_str());
    }
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
    req.viewport_graph = camera_preview_graph_;
    req.global_set = camera_preview_global_set_;

    vk.render_preview(req, light, scene, dt, selected_id_, pool);

    // Signal that at least one render completed — overlay reads < 0
    camera_preview_skip_frames_ = -1;
}

void SceneEditorContext::apply_resize(VulkanContext& vk) {
    viewport_manager_.apply_resize(vk);

    // Camera preview lifecycle — safe here because begin_frame fence already waited
    bool need_preview = (selected_id_ != 0 && !deps_.play->playing() && deps_.scene);
    if (need_preview) {
        auto* node = deps_.scene->find(selected_id_);
        need_preview = (dynamic_cast<CameraNode*>(node) != nullptr);
    }

    if (need_preview && !camera_preview_target_) {
        // Use the same MSAA as the main viewport — pipelines are compiled
        // with this sample count and can't be changed per-viewport.
        camera_preview_target_ = std::make_unique<OffscreenPass>(
            vk.device_ref(), vk.allocator(), 320, 180, vk.msaa_samples());

        // Pipeline-as-data: pp_config + per-call pass mask both flow from
        // editor_preview.fjgraph. If the file fails to load (parse error,
        // missing) we keep the preview disabled rather than fall back to
        // a hand-rolled config — surfacing the failure beats hiding it.
        camera_preview_graph_ = vk.get_or_load_graph("assets/render_graphs/editor_preview.fjgraph");
        if (camera_preview_graph_ == nullptr) {
            camera_preview_target_.reset();
            return;
        }
        PostProcessConfig preview_config = pp_config_from_graph(*camera_preview_graph_);

        PostProcessResources::PassRefs refs{
            vk.hiz_pass(), vk.gtao_pass(), vk.volumetric_fog_pass(),
            vk.tonemap_pass(), vk.ddgi_pass(), vk.ss_gi_pass(), vk.taa_resolve_pass(),
            vk.rt_reflection_pass()};

        camera_preview_pp_ = std::make_unique<PostProcessResources>(
            vk.device(), vk.allocator(), 320, 180, preview_config, refs);
        // Private global set so the preview never rewrites the shared per-frame set.
        camera_preview_global_set_ = vk.gpu().global_bindings().allocate_set();
        camera_preview_node_id_ = selected_id_;
        // Skip rendering for MAX_FRAMES_IN_FLIGHT frames to let all in-flight
        // command buffers drain before the preview's descriptors are first used
        camera_preview_skip_frames_ = 3;
    } else if (!need_preview && camera_preview_target_) {
        // Must wait for all in-flight command buffers that reference
        // the preview's descriptor sets / images before destroying them.
        vk.wait_idle();
        vk.gpu().global_bindings().free_set(camera_preview_global_set_);
        camera_preview_global_set_ = VK_NULL_HANDLE;
        camera_preview_pp_.reset();
        camera_preview_target_.reset();
        camera_preview_node_id_ = 0;
        camera_preview_skip_frames_ = 0;
        camera_preview_graph_ = nullptr;
    }
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

    // Materials the mesh assigns to its own slots take precedence over the
    // fallback picked above.
    if (auto* node = deps_.scene->find(selected_id_)) {
        if (auto* mc = node->components.get<MeshComponent>()) {
            mc->slot_materials = deps_.resource_manager->load_slot_materials(
                model_handle, deps_.project_mgr->active());
        }
    }

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

    // A primitive is solid out of the box. Its shape is known exactly, so
    // making it collide is free, and a cube you have to configure before it
    // stops things falling through is a worse default than one you delete.
    // No rigidbody: a collider without one is static, which is what level
    // geometry wants. Add a Rigidbody to make it fall.
    if (auto* node = deps_.scene->find_spatial(selected_id_)) {
        auto& col = node->components.add<ColliderComponent>();
        fit_collider_to_primitive(col, node->source);
    }
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

namespace {

// Smallest positive distance along -Y from `from_y` to the triangle, if the
// downward ray through (x, z) crosses it. Mirrors the ray/triangle test used
// for picking, specialised to a fixed direction.
bool ray_down_hits_triangle(const glm::vec3& origin,
                            const glm::vec3& a, const glm::vec3& b,
                            const glm::vec3& c, float& out_t) {
    constexpr glm::vec3 DIR{0.0f, -1.0f, 0.0f};
    constexpr float EPS = 1e-7f;

    const glm::vec3 e1 = b - a;
    const glm::vec3 e2 = c - a;
    const glm::vec3 pv = glm::cross(DIR, e2);
    const float det = glm::dot(e1, pv);
    if (std::abs(det) < EPS) return false; // ray parallel to the triangle

    const float inv_det = 1.0f / det;
    const glm::vec3 tv = origin - a;
    const float u = glm::dot(tv, pv) * inv_det;
    if (u < 0.0f || u > 1.0f) return false;

    const glm::vec3 qv = glm::cross(tv, e1);
    const float v = glm::dot(DIR, qv) * inv_det;
    if (v < 0.0f || u + v > 1.0f) return false;

    const float t = glm::dot(e2, qv) * inv_det;
    if (t < 0.0f) return false; // behind the start point
    out_t = t;
    return true;
}

// Downward distance from `origin` to a world AABB's top face, for models that
// carry no collision geometry.
bool ray_down_hits_aabb(const glm::vec3& origin, const AABB& box, float& out_t) {
    if (origin.x < box.min.x || origin.x > box.max.x) return false;
    if (origin.z < box.min.z || origin.z > box.max.z) return false;
    if (box.max.y > origin.y) return false; // not below the start point
    out_t = origin.y - box.max.y;
    return true;
}

} // namespace

void SceneEditorContext::snap_selection_to_floor() {
    auto* obj = deps_.scene->find_spatial(selected_id_);
    if (!obj) return;

    auto* mesh = obj->components.get<MeshComponent>();
    if (!mesh || !mesh->model || !mesh->model->local_aabb().valid()) return;

    const AABB self_box = mesh->model->local_aabb().transform(obj->world_matrix_);

    // Cast from the centre of the object's footprint, starting at its underside.
    const glm::vec3 origin{(self_box.min.x + self_box.max.x) * 0.5f,
                           self_box.min.y,
                           (self_box.min.z + self_box.max.z) * 0.5f};

    // Everything parented under the selection moves with it, so it can't be floor.
    auto is_selected_or_descendant = [&](const Node& n) {
        uint32_t walk = n.id;
        while (walk != 0) {
            if (walk == selected_id_) return true;
            const auto* node = deps_.scene->find(walk);
            if (!node || node->parent_id < 0) break;
            walk = static_cast<uint32_t>(node->parent_id);
        }
        return false;
    };

    float best_t = std::numeric_limits<float>::max();

    for (const auto& other : deps_.scene->objects()) {
        if (is_selected_or_descendant(*other)) continue;

        auto* other_mesh = other->components.get<MeshComponent>();
        if (!other_mesh || !other_mesh->model) continue;
        if (!other_mesh->model->local_aabb().valid()) continue;

        const auto* other_spatial = other->as_spatial();
        if (!other_spatial) continue;

        const glm::mat4& world = other_spatial->world_matrix_;
        const AABB other_box = other_mesh->model->local_aabb().transform(world);

        // Reject anything the downward ray cannot reach before touching triangles.
        float box_t = 0.0f;
        if (!ray_down_hits_aabb(origin, other_box, box_t)) continue;
        if (box_t >= best_t) continue;

        const auto& verts = other_mesh->model->collision_vertices();
        const auto& indices = other_mesh->model->collision_indices();

        if (verts.empty() || indices.size() < 3) {
            best_t = box_t; // no triangles to refine against — rest on the bounds
            continue;
        }

        for (size_t i = 0; i + 2 < indices.size(); i += 3) {
            const glm::vec3 a{world * glm::vec4(verts[indices[i + 0]], 1.0f)};
            const glm::vec3 b{world * glm::vec4(verts[indices[i + 1]], 1.0f)};
            const glm::vec3 c{world * glm::vec4(verts[indices[i + 2]], 1.0f)};

            float t = 0.0f;
            if (ray_down_hits_triangle(origin, a, b, c, t) && t < best_t) {
                best_t = t;
            }
        }
    }

    if (best_t == std::numeric_limits<float>::max()) return; // nothing underneath

    const Transform before = obj->transform;
    Transform after = before;
    after.position.y -= best_t;
    if (after.position == before.position) return;

    command_history().execute(std::make_unique<TransformCommand>(
        *deps_.scene, obj->id, before, after, obj->name));
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
