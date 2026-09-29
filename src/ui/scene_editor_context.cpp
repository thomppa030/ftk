#include "ui/scene_editor_context.hpp"
#include "ui/kit/icons.hpp"
#include "ui/kit/button.hpp"
#include "ui/kit/choice.hpp"
#include "ui/kit/field.hpp"
#include "ui/kit/overlay.hpp"
#include "ui/kit/text_field.hpp"
#include "ui/kit/viewport_toolbar.hpp"
#include "ui/theme.hpp"
#include "ui/viewport_view.hpp"
#include "environment/environment_subsystem.hpp"
#include "ui/dockspace_layout.hpp"
#include "ui/icons_lc.hpp"
#include "core/base_path.hpp"
#include "core/clock.hpp"
#include "core/commands.hpp"
#include "core/input.hpp"
#include "core/log.hpp"
#include "core/play_controller.hpp"
#include "core/project_manager.hpp"
#include "renderer/gpu/window.hpp"
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
#include "scene/scene.hpp"
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
#include "physics/physics_subsystem.hpp"
#include "renderer/components/terrain_component.hpp"
#include "renderer/resources/terrain_asset.hpp"
#include "ui/inspector_widgets.hpp"

#include <ImGuizmo.h>

#include <glm/gtc/type_ptr.hpp>

#include <cmath>
#include <cstdio>
#include <span>
#include <limits>

#include <filesystem>
#include <format>

namespace fjell {

namespace {

// The triangles the editor treats as an object's visible surface. Full-detail
// collision geometry is what the render mesh was imported from, so it is the
// closest match to what the user sees; the derived collision mesh is the
// fallback for files that never stored the full set. Either pointer may be
// null when the model carries no triangles at all.
struct SurfaceGeometry {
    const std::vector<glm::vec3>* vertices = nullptr;
    const std::vector<uint32_t>* indices = nullptr;

    [[nodiscard]] bool valid() const {
        return vertices && indices && !vertices->empty() && indices->size() >= 3;
    }
};

SurfaceGeometry surface_geometry_of(const Model& model) {
    if (model.has_full_collision() && model.collision_full_indices().size() >= 3) {
        return {&model.collision_full_vertices(), &model.collision_full_indices()};
    }
    if (!model.collision_vertices().empty() && model.collision_indices().size() >= 3) {
        return {&model.collision_vertices(), &model.collision_indices()};
    }
    return {};
}

// Möller–Trumbore ray/triangle test. `dir` need not be normalised; `out_t` is
// in units of `dir`. Both windings hit, since a pick should land on whatever
// face is under the cursor regardless of which way it faces.
bool ray_hits_triangle(const glm::vec3& origin, const glm::vec3& dir,
                       const glm::vec3& a, const glm::vec3& b,
                       const glm::vec3& c, float& out_t) {
    constexpr float EPS = 1e-7f;

    const glm::vec3 e1 = b - a;
    const glm::vec3 e2 = c - a;
    const glm::vec3 pv = glm::cross(dir, e2);
    const float det = glm::dot(e1, pv);
    if (std::abs(det) < EPS) return false; // ray parallel to the triangle

    const float inv_det = 1.0f / det;
    const glm::vec3 tv = origin - a;
    const float u = glm::dot(tv, pv) * inv_det;
    if (u < 0.0f || u > 1.0f) return false;

    const glm::vec3 qv = glm::cross(tv, e1);
    const float v = glm::dot(dir, qv) * inv_det;
    if (v < 0.0f || u + v > 1.0f) return false;

    const float t = glm::dot(e2, qv) * inv_det;
    if (t < 0.0f) return false; // behind the start point
    out_t = t;
    return true;
}

// Slab test against a world AABB. `out_t` is the entry distance, or the exit
// distance when the ray starts inside the box.
bool ray_hits_aabb(const glm::vec3& origin, const glm::vec3& dir,
                   const AABB& box, float& out_t) {
    const glm::vec3 inv_dir = 1.0f / dir;
    const glm::vec3 t1 = (box.min - origin) * inv_dir;
    const glm::vec3 t2 = (box.max - origin) * inv_dir;
    const glm::vec3 t_min_v = glm::min(t1, t2);
    const glm::vec3 t_max_v = glm::max(t1, t2);
    const float t_enter = std::max({t_min_v.x, t_min_v.y, t_min_v.z});
    const float t_exit  = std::min({t_max_v.x, t_max_v.y, t_max_v.z});
    if (t_enter > t_exit || t_exit < 0.0f) return false;
    out_t = t_enter >= 0.0f ? t_enter : t_exit;
    return true;
}

// Closest distance along the ray to the object's surface, if the ray hits it.
// The bounding box is only a broad phase: a hit on it means nothing unless a
// triangle confirms it, since boxes of large or rotated meshes enclose plenty
// of empty space and neighbouring objects. Models without triangles fall back
// to the box itself, which is the best information there is for them.
bool ray_hits_model(const glm::vec3& origin, const glm::vec3& dir,
                    const Model& model, const glm::mat4& world,
                    float t_limit, float& out_t) {
    if (!model.local_aabb().valid()) return false;

    float box_t = 0.0f;
    if (!ray_hits_aabb(origin, dir, model.local_aabb().transform(world), box_t)) return false;
    if (box_t >= t_limit) return false;

    const SurfaceGeometry geo = surface_geometry_of(model);
    if (!geo.valid()) {
        out_t = box_t;
        return true;
    }

    const auto& verts = *geo.vertices;
    const auto& indices = *geo.indices;
    float best_t = t_limit;
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const glm::vec3 a{world * glm::vec4(verts[indices[i + 0]], 1.0f)};
        const glm::vec3 b{world * glm::vec4(verts[indices[i + 1]], 1.0f)};
        const glm::vec3 c{world * glm::vec4(verts[indices[i + 2]], 1.0f)};

        float t = 0.0f;
        if (ray_hits_triangle(origin, dir, a, b, c, t) && t < best_t) {
            best_t = t;
        }
    }
    if (best_t >= t_limit) return false;
    out_t = best_t;
    return true;
}

// Skinned counterpart of ray_hits_model. The surface is posed on the CPU
// first, using the animator's palette when it drives the mesh and the bind
// pose otherwise, exactly as the renderer chooses. A posed character can
// reach well outside its bind-pose bounds, so there is no box broad phase;
// scenes hold few enough skinned meshes that posing each one per click is
// cheap. `scratch` holds the posed positions between calls.
bool ray_hits_skeletal_model(const glm::vec3& origin, const glm::vec3& dir,
                             const SkeletalModel& model,
                             const AnimatorComponent* animator,
                             const glm::mat4& world, float t_limit,
                             std::vector<glm::vec3>& scratch, float& out_t) {
    static const std::vector<glm::mat4> BIND_POSE;
    const auto& palette = (animator && animator->drives_pose())
        ? animator->skin_matrices() : BIND_POSE;
    model.pose_surface(palette, scratch);

    const auto& indices = model.surface_indices();
    float best_t = t_limit;
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const glm::vec3 a{world * glm::vec4(scratch[indices[i + 0]], 1.0f)};
        const glm::vec3 b{world * glm::vec4(scratch[indices[i + 1]], 1.0f)};
        const glm::vec3 c{world * glm::vec4(scratch[indices[i + 2]], 1.0f)};

        float t = 0.0f;
        if (ray_hits_triangle(origin, dir, a, b, c, t) && t < best_t) {
            best_t = t;
        }
    }
    if (best_t >= t_limit) return false;
    out_t = best_t;
    return true;
}

// Radius of the sphere the camera frames when double-clicking `obj`: the
// farthest surface point from the node origin, in world units.
float focus_radius_of(const Node3D& obj, std::vector<glm::vec3>& scratch) {
    const glm::vec3 center = glm::vec3(obj.world_matrix_[3]);
    float max_r2 = 0.0f;
    auto extend = [&](const glm::vec3& local) {
        const auto wv = glm::vec3(obj.world_matrix_ * glm::vec4(local, 1.0f));
        max_r2 = std::max(max_r2, glm::dot(wv - center, wv - center));
    };

    if (const auto* mesh = obj.components.get<MeshComponent>();
        mesh && mesh->model) {
        for (const auto& v : mesh->model->collision_vertices()) extend(v);
    } else if (const auto* skel = obj.components.get<SkeletalMeshComponent>();
               skel && skel->skeletal_model) {
        const auto* animator = obj.components.get<AnimatorComponent>();
        static const std::vector<glm::mat4> BIND_POSE;
        skel->skeletal_model->pose_surface(
            (animator && animator->drives_pose()) ? animator->skin_matrices() : BIND_POSE,
            scratch);
        for (const auto& v : scratch) extend(v);
    }

    return max_r2 > 0.0f ? std::max(std::sqrt(max_r2), 0.1f) : 1.0f;
}

} // namespace

SceneEditorContext::SceneEditorContext(SceneEditorDeps deps)
    : deps_{deps} {}

void SceneEditorContext::init() {
    viewport_manager_.init(deps_.vulkan_context->device_ref(),
                           deps_.vulkan_context->allocator(),
                           deps_.vulkan_context->msaa_samples());
    viewport_manager_.add_viewport(ViewportSource::final_color, "Viewport");
    // The Scene draws a toolbar of its own, with its tools and Play.
    viewport_manager_.set_preview_toolbar(false);

    hierarchy_panel_.init(deps_.scene, &viewport_manager_, &selected_id_, &command_history());
    properties_panel_.set_environment(deps_.environment);
    properties_panel_.init(deps_.scene, &selected_id_,
                           &deps_.script_subsystem->manager(), &command_history(),
                           &deps_.vulkan_context->grid_pass().settings(),
                           deps_.vulkan_context->pipeline_cache(),
                           &deps_.vulkan_context->bindless_textures(),
                           deps_.resource_manager, deps_.project_mgr,
                           deps_.content_browser,
                           deps_.icon_cache, &sculpt_tool_);

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
    connections_.push_back(hierarchy_panel_.on_delete_selection.bind(
        [this]() { delete_selection(); }));
    connections_.push_back(hierarchy_panel_.on_duplicate.bind(
        [this]() { duplicate_selection(); }));
    connections_.push_back(hierarchy_panel_.on_instantiate_preset.bind(
        [this](const std::string& rel_path, int32_t parent_id) {
            const auto* project = deps_.project_mgr ? deps_.project_mgr->active() : nullptr;
            auto resolved = base_path::resolve_asset(rel_path, project);
            auto result = PresetTemplate::load(resolved, *deps_.resource_manager, project);
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
    stats_panel().set_simulation(&deps_.play->step_stats());
    history_panel().init(&command_history());
    net_panel_.init(deps_.net_manager);
}

void SceneEditorContext::shutdown() {
    // Clean up preview resources (safe — called after GPU idle)
    camera_preview_pp_.reset();
    camera_preview_target_.reset();
    camera_preview_node_id_ = 0;

    viewport_manager_.release_textures();
    connections_.clear();
}

void SceneEditorContext::draw(float dt) {
    viewport_manager_.draw(*deps_.vulkan_context, *deps_.input, dt);

    // Camera preview PIP overlay (only after preview has rendered at least once)
    if (camera_preview_skip_frames_ < 0 && camera_preview_pp_ && camera_preview_pp_->post()) {
        auto* vp = viewport_manager_.active_panel();
        if (vp && vp->img_size.x > 0 && vp->img_size.y > 0) {
            auto tex = camera_preview_pp_->post()->imgui_texture(
                deps_.vulkan_context->post_pass().output_sampler());
            if (tex) {
                bool visible = ImGui::Begin(vp->title.c_str());
                if (visible) {
                    float preview_w = 320.0f;
                    float preview_h = 180.0f;

                    // Scale down if viewport is small
                    float max_w = vp->img_size.x * 0.35f;
                    if (preview_w > max_w) {
                        float scale = max_w / preview_w;
                        preview_w *= scale;
                        preview_h *= scale;
                    }

                    const ImVec2 min{vp->img_pos.x + vp->img_size.x - preview_w - ui::OVERLAY_INSET,
                                     vp->img_pos.y + vp->img_size.y - preview_h - ui::OVERLAY_INSET};
                    const ImVec2 max{min.x + preview_w, min.y + preview_h};

                    // The picture alone with a black edge like the pills',
                    // and the camera's name on a tag in its corner (sheet 33).
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    const float rounding = ImGui::GetStyle().FrameRounding;
                    dl->AddImageRounded(tex, min, max, {0.0f, 0.0f}, {1.0f, 1.0f}, IM_COL32_WHITE, rounding);
                    dl->AddRect({min.x - 1.0f, min.y - 1.0f}, {max.x + 1.0f, max.y + 1.0f},
                                ImGui::GetColorU32(theme::border()), rounding);
                    if (auto* cam_node = dynamic_cast<CameraNode*>(deps_.scene->find(selected_id_))) {
                        constexpr float TAG_INSET = 6.0f;
                        const ui::OverlayPiece name[] = {{cam_node->name}};
                        ui::corner_note(dl, {min.x + TAG_INSET, min.y + TAG_INSET}, ui::Corner::TopLeft,
                                        {.icon = ui::icon::camera,
                                         .line = name,
                                         .ink = theme::text(),
                                         .icon_ink = theme::category(theme::Category::Camera)});
                    }
                }
                ImGui::End();
            }
        }
    }

    // The click that gives the game back input focus (after Shift+Esc) has
    // to be consumed here, before picking/gizmo below get a look at it —
    // draw_play_toolbar() runs later in this same function and would
    // otherwise detect that same click one frame too late, after picking
    // already treated it as a selection attempt.
    check_viewport_recapture_click();

    // Gizmo and picking on the active viewport — suppressed while the game
    // has input focus (play started, or the player just clicked back into
    // the viewport above) so a click meant for the game can never also
    // select a node and drop a gizmo on it. Shift+Esc hands both back along
    // with everything else editor input reaches.
    //
    // The gizmo runs first so that its hit test is for this frame's mouse
    // position and the click that grabs a handle is decided before picking
    // gets to see it; the other way round a click on a handle the mouse had
    // only just reached went to the mesh behind it.
    auto* vp = viewport_manager_.active_panel();
    bool editor_owns_viewport_input = !deps_.play->input_focused();
    bool gizmo_has_mouse = false;
    if (vp && editor_owns_viewport_input) {
        gizmo_has_mouse = draw_gizmo(*vp);
    }
    // With a brush chosen in the selected terrain's inspector, the viewport
    // sculpts wherever the gizmo did not claim the mouse. The gizmo keeps
    // first claim so the terrain node itself can still be moved.
    const bool sculpting = selected_terrain() != nullptr && sculpt_tool_.active();
    if (vp && editor_owns_viewport_input && !gizmo_has_mouse) {
        if (sculpting) {
            draw_sculpt(*vp);
        } else if (vp->hovered) {
            draw_picking(*vp);
        }
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

    if (focus_viewport_frames_ > 0) {
        if (--focus_viewport_frames_ == 0) {
            ImGui::SetWindowFocus("Viewport");
        }
    }

    draw_shared_panels(dt);
    net_panel_.draw(dt, ctx_title("Network").c_str());
}

void SceneEditorContext::check_viewport_recapture_click() {
    if (!deps_.play->playing() || deps_.play->input_focused()) return;

    auto* vp = viewport_manager_.active_panel();
    if (!vp || !vp->hovered) return;

    // vp->hovered was computed inside viewport_manager_.draw()'s own
    // Begin/End earlier this frame and is still valid to read here;
    // IsMouseClicked reads the raw mouse state and needs no window scope,
    // so this works even though we are not inside that window right now.
    if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;

    auto mouse = ImGui::GetMousePos();
    if (mouse.x < vp->img_pos.x || mouse.x >= vp->img_pos.x + vp->img_size.x ||
        mouse.y < vp->img_pos.y || mouse.y >= vp->img_pos.y + vp->img_size.y) {
        return;
    }

    deps_.play->capture_input();
}

namespace {

// The world-space ray under the mouse, through the viewport's camera. False
// when the panel has no image to be over.
bool viewport_mouse_ray(const ViewportPanel& vp, glm::vec3& origin, glm::vec3& dir) {
    if (vp.img_size.x <= 0.0f || vp.img_size.y <= 0.0f) return false;
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

    origin = glm::vec3(world_near);
    dir = glm::normalize(glm::vec3(world_far - world_near));
    return true;
}

} // namespace

TerrainComponent* SceneEditorContext::selected_terrain() const {
    auto* node = deps_.scene->find_spatial(selected_id_);
    if (node == nullptr) return nullptr;
    auto* terrain = node->components.get<TerrainComponent>();
    return (terrain != nullptr && terrain->terrain != nullptr) ? terrain : nullptr;
}

void SceneEditorContext::draw_sculpt(ViewportPanel& vp) {
    auto* node = deps_.scene->find_spatial(selected_id_);
    auto* comp = selected_terrain();
    if (node == nullptr || comp == nullptr) return;

    TerrainSculptTool::Frame frame{};
    frame.terrain = comp->terrain;
    frame.terrain_origin = comp->terrain->world_min_corner(node->world_matrix_);
    frame.node_id = node->id;
    frame.brush.ray_valid =
        vp.hovered && viewport_mouse_ray(vp, frame.brush.ray_origin, frame.brush.ray_dir);
    frame.brush.mouse_down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    // A press on a toolbar button over the image is that button's, not a
    // stroke. The image itself carries no item id, so it never counts here.
    frame.brush.may_begin_stroke = vp.hovered && !ImGui::IsAnyItemHovered();
    frame.brush.keys_allowed = vp.hovered || vp.focused;
    frame.brush.dt = ImGui::GetIO().DeltaTime;
    frame.brush.debug = deps_.vulkan_context->debug_draw_queue();
    frame.upload = &deps_.vulkan_context->gpu_device().upload();
    frame.physics = deps_.physics;
    frame.history = &command_history();
    sculpt_tool_.update(frame);
}

void SceneEditorContext::draw_picking(ViewportPanel& vp) {
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        glm::vec3 ray_origin{0.0f};
        glm::vec3 ray_dir{0.0f};
        if (!viewport_mouse_ray(vp, ray_origin, ray_dir)) return;

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
        std::vector<glm::vec3> posed;

        for (const auto& obj : deps_.scene->objects()) {
            const auto* spatial = obj->as_spatial();
            if (!spatial) continue;

            float t = 0.0f;
            bool hit = false;
            if (const auto* mesh = obj->components.get<MeshComponent>();
                mesh && mesh->model) {
                hit = ray_hits_model(ray_origin, ray_dir, *mesh->model,
                                     spatial->world_matrix_, closest_t, t);
            } else if (const auto* skel = obj->components.get<SkeletalMeshComponent>();
                       skel && skel->skeletal_model) {
                hit = ray_hits_skeletal_model(ray_origin, ray_dir, *skel->skeletal_model,
                                              obj->components.get<AnimatorComponent>(),
                                              spatial->world_matrix_, closest_t, posed, t);
            } else if (const auto* terrain = obj->components.get<TerrainComponent>();
                       terrain && terrain->terrain) {
                // Terrain has no model to test; its surface is where the
                // mesh shader puts it, which raycast_terrain reproduces.
                glm::vec3 surface{0.0f};
                if (raycast_terrain(*terrain->terrain,
                                    terrain->terrain->world_min_corner(spatial->world_matrix_),
                                    ray_origin, ray_dir, surface)) {
                    const float dist = glm::length(surface - ray_origin);
                    if (dist < closest_t) {
                        hit = true;
                        t = dist;
                    }
                }
            }
            if (hit) {
                closest_t = t;
                hit_id = obj->id;
            }
        }

        if (hit_id != 0) {
            selected_id_ = hit_id;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (auto* hit_obj = deps_.scene->find_spatial(hit_id)) {
                    vp.camera.focus(glm::vec3(hit_obj->world_matrix_[3]),
                                    focus_radius_of(*hit_obj, posed));
                }
            }
        } else {
            selected_id_ = 0;
        }
    }
}

bool SceneEditorContext::draw_gizmo(ViewportPanel& vp) {
    auto* obj = deps_.scene->find_spatial(selected_id_);
    if (!obj) {
        gizmo_dragging_ = false;
        return false;
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
        return false;
    }

    float aspect = vp.img_size.x / std::max(vp.img_size.y, 1.0f);
    auto view = vp.camera.view_matrix();
    auto projection = vp.camera.projection_matrix(aspect);
    projection[1][1] *= -1.0f; // undo Vulkan Y-flip for ImGuizmo

    ImGuizmo::SetRect(vp.img_pos.x, vp.img_pos.y, vp.img_size.x, vp.img_size.y);
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist();

    // ImGuizmo sizes the gizmo as a fraction of the viewport, so the handles
    // shrink with a docked panel while the grab tolerances stay in pixels.
    // Ask for a fixed axis length on screen instead: the fraction is measured
    // along the viewport's longer side.
    constexpr float kGizmoAxisPixels = 100.0f;
    const float longer_side = std::max(std::max(vp.img_size.x, vp.img_size.y), 1.0f);
    ImGuizmo::SetGizmoSizeClipSpace(2.0f * kGizmoAxisPixels / longer_side);

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
        if (tool_ == ViewportTool::rotate) op = ImGuizmo::ROTATE;
        if (tool_ == ViewportTool::scale) op = ImGuizmo::SCALE;
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

    // IsOver(op) re-runs the hit test against the state Manipulate just set
    // up; the argument-less IsOver() only reports the previous frame.
    const bool gizmo_has_mouse = ImGuizmo::IsUsing() || ImGuizmo::IsOver(op);

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
    return gizmo_has_mouse;
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

void SceneEditorContext::draw_play_pill(ImVec2 image_min, ImVec2 image_max) {
    const bool playing = deps_.play->playing();
    const bool paused = deps_.play->paused();
    auto pill = ui::ViewportPill("##play", image_min, image_max, ui::PillPlace::TopCenter);

    // Before Play, how the game starts: offline, hosting or joining. The
    // engine never acts on it; the game's script reads it in begin_play.
    if (!playing && deps_.net_manager != nullptr) {
        static constexpr const char* MODES[] = {"Offline", "Host", "Join"};
        int mode = static_cast<int>(deps_.net_manager->launch_mode());
        ImGui::SetNextItemWidth(90.0f);
        if (ui::choice("##net_mode", mode, MODES).changed) {
            deps_.net_manager->set_launch_mode(static_cast<NetLaunchMode>(mode));
        }
        ImGui::SetItemTooltip("How the game starts: offline, as the host, or joining one");
        if (deps_.net_manager->launch_mode() == NetLaunchMode::join) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120.0f);
            std::string address = deps_.net_manager->join_address();
            if (ui::text_field("##net_address", address).committed) deps_.net_manager->set_join_address(address);
            ImGui::SetItemTooltip("The host's address");
        }
        ImGui::SameLine();
    }

    if (ui::icon_button("##play", playing ? ui::icon::stop : ui::icon::play, playing ? "Stop (Ctrl P)" : "Play (Ctrl P)",
                        playing ? ui::ButtonKind::Ghost : ui::ButtonKind::Secondary)) {
        deps_.play->toggle();
    }
    if (!playing) return;

    ImGui::SameLine();
    if (ui::toggle_button("##pause", ui::icon::pause, paused, paused ? "Resume (Ctrl Shift P)" : "Pause (Ctrl Shift P)")) {
        deps_.play->toggle_pause();
    }
    ImGui::SameLine();
    if (ui::icon_button("##step", ui::icon::step_frame, "Step one frame (Ctrl Alt P)")) deps_.play->step();
    ImGui::SameLine();
    const bool ejected = deps_.play->ejected();
    if (ui::toggle_button("##eject", ejected ? ui::icon::possess_camera : ui::icon::eject_camera, ejected,
                          ejected ? "Back to the game's camera (F10)" : "Leave the game's camera (F10)")) {
        deps_.play->toggle_eject();
    }
    ImGui::SameLine();
    float scale = deps_.play->time_scale();
    ImGui::SetNextItemWidth(72.0f);
    if (ui::drag("##time_scale", scale, {.speed = 0.01f, .min = 0.1f, .max = 4.0f, .unit = ui::Unit::Times}).changed) {
        deps_.play->set_time_scale(scale);
    }
    ImGui::SetItemTooltip("Time scale");
}

void SceneEditorContext::draw_play_toolbar() {
    auto* vp = viewport_manager_.active_panel();
    if (!vp) return;

    // Must draw inside the viewport window's Begin/End
    if (!ImGui::Begin(vp->title.c_str())) {
        ImGui::End();
        return;
    }

    const bool playing = deps_.play->playing();
    const ImVec2 min{vp->img_pos.x, vp->img_pos.y};
    const ImVec2 max{vp->img_pos.x + vp->img_size.x, vp->img_pos.y + vp->img_size.y};

    if (playing) {
        scene_image_rect_.pos = vp->img_pos;
        scene_image_rect_.size = vp->img_size;

        // HUD composited on top of the docked panel's own scene image,
        // which viewport_manager_.draw() already drew before this runs —
        // see draw_play_overlay for the fullscreen equivalent.
        auto hud_tex = deps_.play->hud_texture();
        if (hud_tex) {
            ImGui::SetCursorScreenPos(min);
            ImGui::Image(hud_tex, {vp->img_size.x, vp->img_size.y});
        }

        // The click that re-captures focus after a Shift+Esc release is
        // detected earlier in draw(), by check_viewport_recapture_click() —
        // before this function runs, so picking/gizmo see the new focus
        // state on the same frame as the click that caused it.

        // How input reaches the game, bottom-left of the image, out of the
        // toolbar's way.
        static constexpr ui::OverlayPiece RELEASE[] = {{"Shift", true}, {"Esc", true}, {"releases input"}};
        static constexpr ui::OverlayPiece GIVE[] = {{"Click to give the game input"}};
        ui::corner_note(ImGui::GetWindowDrawList(), {min.x + ui::OVERLAY_INSET, max.y - ui::OVERLAY_INSET},
                        ui::Corner::BottomLeft,
                        {.line = deps_.play->input_focused() ? std::span<const ui::OverlayPiece>(RELEASE)
                                                             : std::span<const ui::OverlayPiece>(GIVE),
                         .ink = theme::text_secondary()});
    }

    // Left: the gizmo's tool, then what is drawn over the scene and how
    // things snap (sheet 28).
    if (auto pill = ui::ViewportPill("##tools", min, max, ui::PillPlace::TopLeft)) {
        if (ui::toggle_button("##move", ui::icon::move_tool, tool_ == ViewportTool::move, "Move (W)")) {
            tool_ = ViewportTool::move;
        }
        ImGui::SameLine();
        if (ui::toggle_button("##rotate", ui::icon::rotate_tool, tool_ == ViewportTool::rotate, "Rotate (E)")) {
            tool_ = ViewportTool::rotate;
        }
        ImGui::SameLine();
        if (ui::toggle_button("##scale", ui::icon::scale_tool, tool_ == ViewportTool::scale, "Scale (R)")) {
            tool_ = ViewportTool::scale;
        }
        if (deps_.vulkan_context != nullptr) {
            auto& grid = deps_.vulkan_context->grid_pass().settings();
            ImGui::SameLine(0.0f, theme::GAP_M);
            if (ui::toggle_button("##grid", ui::icon::grid, grid.visible, "Grid")) grid.visible = !grid.visible;
            ImGui::SameLine();
            if (ui::toggle_button("##snap", ui::icon::snap, grid.snap_enabled, "Snap to grid (Ctrl inverts)")) {
                grid.snap_enabled = !grid.snap_enabled;
            }
            ImGui::SameLine();
            const bool colliders = deps_.vulkan_context->show_colliders();
            if (ui::toggle_button("##colliders", ui::icon::collision, colliders, "Colliders")) {
                deps_.vulkan_context->set_show_colliders(!colliders);
            }
        }
    }

    draw_play_pill(min, max);

    ImGui::End();
}

bool SceneEditorContext::has_unsaved_changes() const {
    return deps_.scene->is_dirty();
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

    bool navigating = deps_.input->raw_mouse_button_held(InputSource::mouse_right);
    if (navigating) return false;

    // Gizmo mode shortcuts
    if (ImGui::IsKeyPressed(ImGuiKey_W)) { tool_ = ViewportTool::move; return true; }
    if (ImGui::IsKeyPressed(ImGuiKey_E)) { tool_ = ViewportTool::rotate; return true; }
    if (ImGui::IsKeyPressed(ImGuiKey_R)) { tool_ = ViewportTool::scale; return true; }
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

    {
        ui::ViewportWindow play_view("##PlayView", nullptr, flags);

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

        // Play, pause, step and time scale, top centre: the same pill drawn
        // over the docked viewport panel while editing — see draw_play_toolbar.
        draw_play_pill({viewport->WorkPos.x, viewport->WorkPos.y},
                       {viewport->WorkPos.x + viewport->WorkSize.x, viewport->WorkPos.y + viewport->WorkSize.y});

        // The frame time, top right in the mono face, averaged over half a
        // second. Kept as an ImGui draw instead of a {fps} game-UI binding so
        // the player-facing HUD doesn't rebuild its layout twice a second
        // just to show a dev stat.
        {
            play_fps_accum_s_ += ImGui::GetIO().DeltaTime;
            ++play_fps_count_;
            if (play_fps_accum_s_ >= 0.5) {
                play_fps_value_ = static_cast<float>(
                    static_cast<double>(play_fps_count_) / play_fps_accum_s_);
                play_fps_accum_s_ = 0.0;
                play_fps_count_ = 0;
            }
            if (play_fps_value_ > 0.0f) {
                const std::string frame_time =
                    std::format("{:.1f} ms \xc2\xb7 {:.0f} fps", 1000.0f / play_fps_value_, play_fps_value_);
                const ui::OverlayPiece line[] = {{frame_time}};
                ImGui::PushFont(theme::mono_font(), 0.0f);
                ui::corner_note(ImGui::GetWindowDrawList(),
                                {viewport->WorkPos.x + viewport->WorkSize.x - ui::OVERLAY_INSET,
                                 viewport->WorkPos.y + ui::OVERLAY_INSET},
                                ui::Corner::TopRight, {.line = line, .ink = theme::text()});
                ImGui::PopFont();
            }
        }

        // Script-submitted debug text, over the whole play view. Still inside
        // ##PlayView's Begin/End, so this uses that window's draw list.
        draw_screen_debug(ImGui::GetWindowDrawList(),
                          {viewport->WorkPos.x, viewport->WorkPos.y},
                          {viewport->WorkSize.x, viewport->WorkSize.y});
    }

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

    // Watches and messages share one plate under the toolbar's pills, in
    // the mono face (sheet 33): a watch's name in the secondary colour and
    // its value in the script's, then the messages. Each world-anchored
    // line gets a small plate of its own.
    ImGui::PushFont(theme::mono_font(), 0.0f);
    const float line_h = ImGui::GetTextLineHeightWithSpacing();
    constexpr float PAD_X = 8.0f;
    constexpr float PAD_Y = 5.0f;
    constexpr float COLUMN_GAP = 12.0f;
    const auto ink = [](const glm::vec4& c) { return ImGui::ColorConvertFloat4ToU32({c.r, c.g, c.b, c.a}); };

    if (!watches.empty() || !messages.empty()) {
        float name_w = 0.0f;
        float value_w = 0.0f;
        for (const auto* e : watches) {
            name_w = std::max(name_w, ImGui::CalcTextSize(e->name.c_str()).x);
            value_w = std::max(value_w, ImGui::CalcTextSize(e->text.c_str()).x);
        }
        float width = watches.empty() ? 0.0f : name_w + COLUMN_GAP + value_w;
        for (const auto* e : messages) width = std::max(width, ImGui::CalcTextSize(e->text.c_str()).x);
        const float height = line_h * static_cast<float>(watches.size() + messages.size()) -
                             ImGui::GetStyle().ItemSpacing.y;

        const ImVec2 min{origin.x + ui::OVERLAY_INSET, ui::pill_row_bottom({origin.x, origin.y}) + theme::GAP_M};
        ui::overlay_plate(dl, min, {min.x + width + PAD_X * 2.0f, min.y + height + PAD_Y * 2.0f});
        float y = min.y + PAD_Y;
        const float x = min.x + PAD_X;
        for (const auto* e : watches) {
            dl->AddText({x, y}, ImGui::GetColorU32(theme::text_secondary()), e->name.c_str());
            dl->AddText({x + name_w + COLUMN_GAP, y}, ink(e->color), e->text.c_str());
            y += line_h;
        }
        for (const auto* e : messages) {
            dl->AddText({x, y}, ink(e->color), e->text.c_str());
            y += line_h;
        }
    }
    ImGui::PopFont();

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
    ImGui::PushFont(theme::mono_font(), 0.0f);
    for (const auto* e : world) {
        glm::vec4 clip = view_proj * glm::vec4(e->world_pos, 1.0f);
        if (clip.w <= 0.0f) continue;  // behind the camera

        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        if (ndc.x < -1.0f || ndc.x > 1.0f || ndc.y < -1.0f || ndc.y > 1.0f) continue;

        // Projection already carries the Vulkan Y-flip, so NDC y maps straight
        // down the screen the same way x maps across it.
        ImVec2 pos{origin.x + (ndc.x * 0.5f + 0.5f) * size.x,
                   origin.y + (ndc.y * 0.5f + 0.5f) * size.y};

        // Centred on the anchor point, on a plate of its own.
        constexpr float TAG_PAD_X = 6.0f;
        constexpr float TAG_PAD_Y = 1.0f;
        const ImVec2 text_size = ImGui::CalcTextSize(e->text.c_str());
        pos.x = std::floor(pos.x - text_size.x * 0.5f);
        pos.y = std::floor(pos.y);
        ui::overlay_plate(dl, {pos.x - TAG_PAD_X, pos.y - TAG_PAD_Y},
                          {pos.x + text_size.x + TAG_PAD_X, pos.y + text_size.y + TAG_PAD_Y});
        dl->AddText(pos, ink(e->color), e->text.c_str());
    }
    ImGui::PopFont();
}

// --- Viewport ---

std::vector<ViewportRenderRequest> SceneEditorContext::build_render_requests() {
    // Preview is rendered separately via render_previews() — not included here.
    // The Scene's viewports take their grid from the Scene's own setting,
    // which the toolbar and the View menu switch.
    auto requests = viewport_manager_.build_render_requests();
    if (deps_.vulkan_context != nullptr) {
        const bool grid = deps_.vulkan_context->grid_pass().settings().visible;
        for (auto& request : requests) request.show_grid = grid;
    }
    return requests;
}

void SceneEditorContext::render_previews(VulkanContext& vk, Scene* scene, float dt,
                                          const EnvironmentState& environment, ThreadPool& pool) {
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

    vk.render_preview(req, environment, scene, dt, selected_id_, pool);

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
            vk.device_ref(), vk.gpu_device(), vk.allocator(), 320, 180, vk.msaa_samples());

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
            vk.post_pass(), vk.ddgi_pass(), vk.ss_gi_pass(), vk.taa_resolve_pass(),
            vk.rt_reflection_pass()};

        camera_preview_pp_ = std::make_unique<PostProcessResources>(
            vk.gpu_device(), vk.device(), vk.allocator(), 320, 180, preview_config, refs);
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
    if (type == "fog_volume") {
        auto node = std::make_unique<FogVolumeNode>();
        node->name = "Fog Volume";
        // A unit volume is a metre across, which is far too small to see as
        // fog; place it at a size a bank is actually authored at.
        node->transform.scale = glm::vec3(10.0f, 3.0f, 10.0f);
        selected_id_ = deps_.scene->add_node(std::move(node));
        return;
    }
    if (type == "post_process_volume") {
        auto node = std::make_unique<PostProcessVolumeNode>();
        node->name = "Post Process Volume";
        // Big enough to stand in, so the look shows the moment the camera
        // walks into it.
        node->transform.scale = glm::vec3(10.0f);
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
    constexpr glm::vec3 DOWN{0.0f, -1.0f, 0.0f};
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

        const auto* other_spatial = other->as_spatial();
        if (!other_spatial) continue;

        // Terrain has no triangles to walk; its surface is wherever the mesh
        // shader puts it, which raycast_terrain reproduces.
        if (const auto* terrain = other->components.get<TerrainComponent>();
            terrain != nullptr && terrain->terrain != nullptr) {
            glm::vec3 surface{0.0f};
            if (raycast_terrain(*terrain->terrain,
                                terrain->terrain->world_min_corner(other_spatial->world_matrix_),
                                origin, DOWN, surface)) {
                const float t = origin.y - surface.y;
                if (t >= 0.0f && t < best_t) best_t = t;
            }
            continue;
        }

        auto* other_mesh = other->components.get<MeshComponent>();
        if (!other_mesh || !other_mesh->model) continue;
        if (!other_mesh->model->local_aabb().valid()) continue;

        const glm::mat4& world = other_spatial->world_matrix_;
        const AABB other_box = other_mesh->model->local_aabb().transform(world);

        // Reject anything the downward ray cannot reach before touching triangles.
        float box_t = 0.0f;
        if (!ray_down_hits_aabb(origin, other_box, box_t)) continue;
        if (box_t >= best_t) continue;

        const SurfaceGeometry geo = surface_geometry_of(*other_mesh->model);
        if (!geo.valid()) {
            best_t = box_t; // no triangles to refine against — rest on the bounds
            continue;
        }

        const auto& verts = *geo.vertices;
        const auto& indices = *geo.indices;
        for (size_t i = 0; i + 2 < indices.size(); i += 3) {
            const glm::vec3 a{world * glm::vec4(verts[indices[i + 0]], 1.0f)};
            const glm::vec3 b{world * glm::vec4(verts[indices[i + 1]], 1.0f)};
            const glm::vec3 c{world * glm::vec4(verts[indices[i + 2]], 1.0f)};

            float t = 0.0f;
            if (ray_hits_triangle(origin, DOWN, a, b, c, t) && t < best_t) {
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
    // One undo step for the whole selection.
    std::vector<uint32_t> ids(sel.begin(), sel.end());
    auto remove = [this](uint32_t id) {
        return std::make_unique<RemoveObjectCommand>(*deps_.scene, id, &selected_id_,
                                                     [this]() { deps_.vulkan_context->wait_idle(); });
    };
    if (ids.size() == 1) {
        command_history().execute(remove(ids.front()));
    } else {
        auto group = std::make_unique<CommandGroup>("Delete " + std::to_string(ids.size()) + " objects");
        for (uint32_t id : ids) group->add(remove(id));
        command_history().execute(std::move(group));
    }
    hierarchy_panel_.clear_selection();
}


} // namespace fjell
