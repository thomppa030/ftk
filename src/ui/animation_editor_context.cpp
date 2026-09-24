#include "ui/animation_editor_context.hpp"
#include "ui/dockspace_layout.hpp"
#include "animation/anim_notify_registry.hpp"
#include "animation/animation_clip.hpp"
#include "animation/animation_subsystem.hpp"
#include "animation/animator_component.hpp"
#include "scene/component_meta.hpp"
#include "ui/inspectors/script_fields.hpp"
#include "ui/kit/icons.hpp"
#include "ui/kit/tree.hpp"
#include "animation/skeleton.hpp"
#include "core/base_path.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include "renderer/debug_draw.hpp"
#include "renderer/resources/fjmesh_writer.hpp"
#include "core/clock.hpp"
#include "core/input.hpp"
#include "core/log.hpp"
#include "core/project_manager.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/components/skeletal_mesh_component.hpp"
#include "renderer/resources/skeletal_model.hpp"
#include "renderer/vulkan_context.hpp"
#include "scene/scene.hpp"
#include "ui/icon_cache.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>
#include <imgui_internal.h>
#include <imnodes.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace fjell {

namespace fs = std::filesystem;

AnimationEditorContext::AnimationEditorContext(AnimationEditorDeps deps)
    : deps_{deps} {}

void AnimationEditorContext::init() {
    viewport_manager_.init(deps_.vulkan_context->device_ref(),
                           deps_.vulkan_context->allocator(),
                           deps_.vulkan_context->msaa_samples());
    viewport_manager_.add_viewport(ViewportSource::final_color, "Anim Preview");

    timeline_.set_title("Anim Timeline");

    // Create own imnodes context for the state graph
    auto* prev_ctx = ImNodes::GetCurrentContext();
    imnodes_ctx_ = static_cast<void*>(ImNodes::CreateContext());
    ImNodes::SetCurrentContext(static_cast<ImNodesContext*>(imnodes_ctx_));

    ImNodesStyle& style = ImNodes::GetStyle();
    style.Flags |= ImNodesStyleFlags_GridSnapping;
    style.Colors[ImNodesCol_NodeBackground]         = IM_COL32(43, 45, 49, 255);
    style.Colors[ImNodesCol_NodeBackgroundHovered]   = IM_COL32(49, 51, 56, 255);
    style.Colors[ImNodesCol_NodeBackgroundSelected]  = IM_COL32(56, 58, 64, 255);
    style.Colors[ImNodesCol_TitleBar]                = IM_COL32(30, 31, 34, 255);
    style.Colors[ImNodesCol_TitleBarHovered]         = IM_COL32(49, 51, 56, 255);
    style.Colors[ImNodesCol_TitleBarSelected]        = IM_COL32(212, 160, 84, 255);
    style.Colors[ImNodesCol_Link]                    = IM_COL32(212, 160, 84, 200);
    style.Colors[ImNodesCol_LinkHovered]             = IM_COL32(212, 160, 84, 255);
    style.Colors[ImNodesCol_LinkSelected]            = IM_COL32(184, 137, 62, 255);
    style.Colors[ImNodesCol_Pin]                     = IM_COL32(212, 160, 84, 200);
    style.Colors[ImNodesCol_PinHovered]              = IM_COL32(212, 160, 84, 255);
    style.Colors[ImNodesCol_GridBackground]          = IM_COL32(17, 18, 20, 255);
    style.Colors[ImNodesCol_GridLine]                = IM_COL32(43, 45, 49, 100);
    style.Colors[ImNodesCol_GridLinePrimary]         = IM_COL32(43, 45, 49, 180);
    style.NodeCornerRounding = 6.0f;
    style.NodePadding        = ImVec2(10.0f, 6.0f);
    style.LinkThickness      = 2.5f;
    style.PinCircleRadius    = 4.5f;

    ImNodes::SetCurrentContext(prev_ctx);

    // Wire timeline callbacks
    connections_.push_back(timeline_.on_scrub.bind([this](float time) {
        // Scrub the animator to this time
        auto* node = preview_scene_.find(preview_node_id_);
        if (!node) return;
        auto* anim = node->components.get<AnimatorComponent>();
        if (!anim) return;
        anim->set_time(time);
    }));

    connections_.push_back(timeline_.on_stop.bind([this]() {
        auto* node = preview_scene_.find(preview_node_id_);
        if (!node) return;
        auto* anim = node->components.get<AnimatorComponent>();
        if (!anim) return;
        anim->set_time(0.0f);
    }));

    // Event marker selection
    connections_.push_back(timeline_.on_marker_selected.bind([this](int idx) {
        selected_event_index_ = idx;
    }));

    // Right-click on timeline: open Add Notify type picker
    connections_.push_back(timeline_.on_right_click.bind([this](float time) {
        if (!current_clip_) return;
        pending_notify_time_ = time;
        open_add_notify_popup_ = true;
    }));

    // Marker drag: update notify time
    connections_.push_back(timeline_.on_marker_moved.bind([this](int idx, float new_time) {
        if (!current_clip_ || idx < 0 || idx >= static_cast<int>(current_clip_->notifies().size())) return;
        current_clip_->notifies()[idx].time = new_time;
    }));

    init_shared_panels(deps_.vulkan_context);
    history_panel().init(&command_history());
}

void AnimationEditorContext::shutdown() {
    if (imnodes_ctx_) {
        auto* prev_ctx = ImNodes::GetCurrentContext();
        ImNodes::SetCurrentContext(static_cast<ImNodesContext*>(imnodes_ctx_));
        ImNodes::DestroyContext();
        if (prev_ctx != imnodes_ctx_) {
            ImNodes::SetCurrentContext(prev_ctx);
        }
        imnodes_ctx_ = nullptr;
    }
    viewport_manager_.release_textures();
    connections_.clear();
}

void AnimationEditorContext::on_enter() {
    // Could refresh clip list here
}

void AnimationEditorContext::draw(float dt) {
    update_preview(dt);

    // Draw the authored shapes on the posed skeleton. Reading the animator's
    // bone transforms rather than the rest pose is the point — a shape that
    // looks right on a T-pose can be badly wrong once a limb swings.
    if (show_colliders_ && !colliders_.empty()) {
        auto& debug = deps_.vulkan_context->debug_draw();
        auto* node = preview_scene_.find_spatial(preview_node_id_);
        const auto* anim = node != nullptr
            ? node->components.get<AnimatorComponent>() : nullptr;

        for (int i = 0; i < static_cast<int>(colliders_.size()); ++i) {
            auto shape = colliders_[static_cast<size_t>(i)].shape;
            if (anim != nullptr && shape.bone >= 0) {
                const auto& bones = anim->bone_matrices();
                if (static_cast<size_t>(shape.bone) < bones.size()) {
                    // Fold the bone into the shape's own placement, so the
                    // wireframe sits where the body will.
                    auto placed = bones[static_cast<size_t>(shape.bone)] *
                                  glm::translate(glm::mat4(1.0F), shape.position) *
                                  glm::mat4_cast(shape.rotation);
                    shape.position = glm::vec3(placed[3]);
                    shape.rotation = glm::quat_cast(glm::mat3(
                        glm::normalize(glm::vec3(placed[0])),
                        glm::normalize(glm::vec3(placed[1])),
                        glm::normalize(glm::vec3(placed[2]))));
                }
            }
            debug.submit_collision_primitive(shape, i == selected_collider_);
        }
    }

    if (auto* vp = viewport_manager_.active_panel()) {
        auto t = ctx_title("Anim Preview");
        if (vp->title != t) vp->title = std::move(t);
    }
    timeline_.set_title(ctx_title("Anim Timeline"));

    viewport_manager_.draw(*deps_.vulkan_context, *deps_.input, dt);
    timeline_.draw(dt);

    // Add Notify popup (opened by right-click on timeline)
    if (open_add_notify_popup_) {
        ImGui::OpenPopup("##AddNotify");
        open_add_notify_popup_ = false;
    }
    if (ImGui::BeginPopup("##AddNotify")) {
        ImGui::SeparatorText("Add Notify");
        AnimNotifyRegistry::instance().for_each([&](const std::string& name, const NotifyTypeInfo& info) {
            const char* suffix = info.is_state ? " (state)" : "";
            if (ImGui::MenuItem((name + suffix).c_str())) {
                if (current_clip_) {
                    AnimNotifyInstance ni;
                    ni.time = pending_notify_time_;
                    ni.type_name = name;
                    ni.instance = info.create();
                    if (info.is_state) {
                        ni.end_time = std::min(pending_notify_time_ + 0.5f, current_clip_->duration());
                    }
                    current_clip_->notifies().push_back(std::move(ni));
                    sync_markers_from_clip();
                    selected_event_index_ = static_cast<int>(current_clip_->notifies().size()) - 1;
                }
            }
        });
        ImGui::EndPopup();
    }

    draw_bone_tree();
    draw_collider_panel();
    draw_clip_list();
    draw_properties();
    draw_state_graph();
    draw_parameters();

    draw_shared_panels(dt);
}

std::vector<std::string> AnimationEditorContext::docked_window_names() const {
    return {ctx_title("Bone Tree"), ctx_title("Clip List"), ctx_title("History"),
            ctx_title("Anim Parameters"), ctx_title("Anim Preview"),
            ctx_title("State Graph"), ctx_title("Anim Properties"),
            ctx_title("Anim Timeline"), ctx_title("Stats"), ctx_title("Console"),
            ctx_title("Bone Colliders")};
}

void AnimationEditorContext::setup_dockspace(ImGuiID main_area) {
    enum class EPanel { BoneTree, ClipList, History, Parameters, Preview, StateGraph, Properties,
                        Timeline, Stats, Console, Colliders };
    using N = DockNode;
    static const auto layout = N::horizontal_split(0.14f,
        N::vertical_split(0.35f,
            N::leaf({EPanel::BoneTree}),
            N::vertical_split(0.50f,
                N::leaf({EPanel::ClipList, EPanel::History}),
                N::leaf({EPanel::Parameters}))),
        N::horizontal_split(0.78f,
            N::vertical_split(0.68f,
                N::leaf({EPanel::Preview, EPanel::StateGraph}),
                N::leaf({EPanel::Timeline, EPanel::Stats, EPanel::Console})),
            N::leaf({EPanel::Properties, EPanel::Colliders})));
    build_dockspace(main_area, layout, docked_window_names());
}

bool AnimationEditorContext::handle_shortcuts() {
    if (ImGui::IsAnyItemActive()) return false;

    // Space: toggle play
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        if (timeline_.playing()) {
            timeline_.set_playing(false);
            timeline_.on_pause.broadcast();
        } else {
            timeline_.set_playing(true);
            timeline_.on_play.broadcast();
        }
        return true;
    }

    // Delete: remove selected notify
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && current_clip_ &&
        selected_event_index_ >= 0 &&
        selected_event_index_ < static_cast<int>(current_clip_->notifies().size())) {
        current_clip_->notifies().erase(
            current_clip_->notifies().begin() + selected_event_index_);
        selected_event_index_ = -1;
        sync_markers_from_clip();
        return true;
    }

    return false;
}

std::vector<ViewportRenderRequest> AnimationEditorContext::build_render_requests() {
    return preview_render_requests(viewport_manager_.build_render_requests());
}

void AnimationEditorContext::apply_resize(VulkanContext& vk) {
    viewport_manager_.apply_resize(vk);
}

// ── Save ─────────────────────────────────────────────────────────────────

std::string AnimationEditorContext::document() const {
    // The three things this editor saves: the clip's notifies (its tracks
    // are never edited here), the skeleton's bone colliders and the animset.
    nlohmann::json j;
    if (current_clip_) j["notifies"] = current_clip_->notifies_text();
    for (const auto& c : colliders_) {
        const auto& p = c.shape;
        j["colliders"].push_back({c.bone_name, static_cast<int>(p.kind), p.position.x, p.position.y,
                                  p.position.z, p.rotation.x, p.rotation.y, p.rotation.z, p.rotation.w,
                                  p.extents.x, p.extents.y, p.extents.z});
    }
    if (!animset_path_.empty()) j["animset"] = state_machine_.text();
    return j.dump();
}

Result<> AnimationEditorContext::write() {
    std::string failed;

    // Save current clip (events may have been edited)
    if (current_clip_ && !current_clip_path_.empty()) {
        auto resolved = base_path::resolve_asset(current_clip_path_, deps_.project_mgr->active());
        if (auto r = current_clip_->save(resolved); r) {
            FJELL_CORE_INFO("AnimEditor: saved clip {}", current_clip_path_);
        } else {
            failed += "the clip: " + r.error() + ". ";
        }
    }

    // Save bone colliders back to the .fjskel
    if (colliders_dirty_ && !skeleton_path_.empty()) {
        auto resolved = base_path::resolve_asset(skeleton_path_, deps_.project_mgr->active());
        if (rewrite_skeleton_colliders(resolved, colliders_)) {
            if (skeleton_) skeleton_->colliders() = colliders_;
            colliders_dirty_ = false;
            FJELL_CORE_INFO("AnimEditor: saved {} bone colliders to {}",
                            colliders_.size(), skeleton_path_);
        } else {
            failed += "the bone colliders: reimport the skeleton if it predates the collider section. ";
        }
    }

    // Save animset
    if (!animset_path_.empty()) {
        auto resolved = base_path::resolve_asset(animset_path_, deps_.project_mgr->active());
        if (auto r = state_machine_.save(resolved); r) {
            FJELL_CORE_INFO("AnimEditor: saved animset {}", animset_path_);
        } else {
            failed += "the animset: " + r.error() + ". ";
        }
    }

    if (!failed.empty()) return make_error("couldn't save " + failed);
    return {};
}

Result<> AnimationEditorContext::restore(const std::string& text) {
    auto j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object()) return make_error("the animation document isn't JSON");

    if (current_clip_ && j.contains("notifies")) {
        if (auto r = current_clip_->set_notifies_from_text(j["notifies"].get<std::string>()); !r) return r;
        sync_markers_from_clip();
        if (selected_event_index_ >= static_cast<int>(current_clip_->notifies().size())) {
            selected_event_index_ = -1;
        }
    }

    std::vector<BoneCollider> colliders;
    for (const auto& c : j.value("colliders", nlohmann::json::array())) {
        BoneCollider collider;
        collider.bone_name = c.at(0).get<std::string>();
        auto& p = collider.shape;
        p.kind = static_cast<CollisionPrimitiveKind>(c.at(1).get<int>());
        p.position = {c.at(2).get<float>(), c.at(3).get<float>(), c.at(4).get<float>()};
        p.rotation = glm::quat{c.at(8).get<float>(), c.at(5).get<float>(), c.at(6).get<float>(), c.at(7).get<float>()};
        p.extents = {c.at(9).get<float>(), c.at(10).get<float>(), c.at(11).get<float>()};
        colliders.push_back(std::move(collider));
    }
    colliders_ = std::move(colliders);
    colliders_dirty_ = true;
    if (selected_collider_ >= static_cast<int>(colliders_.size())) selected_collider_ = -1;

    if (!animset_path_.empty() && j.contains("animset")) {
        if (auto r = state_machine_.load_text(j["animset"].get<std::string>(), animset_path_); !r) return r;
        build_graph_from_state_machine();
    }
    return {};
}

// ── Open from asset ──────────────────────────────────────────────────────

void AnimationEditorContext::open_skeleton(const std::string& skel_path) {
    skeleton_path_ = skel_path;
    auto resolved = base_path::resolve_asset(skel_path, deps_.project_mgr->active());

    name_ = fs::path(skel_path).stem().string();
    selected_bone_ = -1;
    selected_clip_ = -1;
    current_clip_.reset();
    colliders_.clear();
    selected_collider_ = -1;
    colliders_dirty_ = false;

    // Load the .fjskel as a skeletal model (contains mesh + embedded skeleton)
    auto model_result = deps_.resource_manager->load_skeletal_model(resolved);
    if (!model_result) {
        FJELL_CORE_ERROR("AnimEditor: failed to load skeletal model: {}", model_result.error());
        return;
    }

    auto* skel_model = deps_.resource_manager->get(*model_result);
    if (!skel_model || !skel_model->skeleton()) {
        FJELL_CORE_ERROR("AnimEditor: skeletal model has no embedded skeleton: {}", skel_path);
        return;
    }
    colliders_ = skel_model->skeleton()->colliders();

    // Get skeleton from the model
    skeleton_ = skel_model->skeleton_shared();

    // Scan for .fjanim files in the same directory
    scan_clips();

    // Set up preview scene with the skeletal mesh
    preview_scene_.clear();

    auto node = std::make_unique<SceneNode>();
    node->name = name_;

    auto& smc = node->components.add<SkeletalMeshComponent>();
    smc.skeletal_model_handle = *model_result;
    smc.skeletal_model = skel_model;

    // Default material
    auto tex = deps_.resource_manager->load_texture(
        base_path::resolve("engine_assets/textures/test.png")).value_or(TextureHandle{});
    smc.material_handle = deps_.resource_manager->create_material("anim_preview", tex);
    smc.material = deps_.resource_manager->get(smc.material_handle);

    auto& anim = node->components.add<AnimatorComponent>();
    anim.init(skel_model->skeleton());

    preview_node_id_ = preview_scene_.add_node(std::move(node));

    // Auto-focus camera
    if (auto* cam = viewport_manager_.active_camera()) {
        cam->focus({0.0f, 1.0f, 0.0f}, 2.5f);
    }

    // Play first clip if available
    if (!available_clip_paths_.empty()) {
        load_clip(0);
    }

    document_opened();
    FJELL_CORE_INFO("AnimEditor: opened skeleton {} ({} joints, {} clips)",
                    skel_path, skeleton_->num_joints(),
                    static_cast<int>(available_clip_paths_.size()));
}

void AnimationEditorContext::open_clip(const std::string& anim_path) {
    // Find the skeleton in the same directory
    auto resolved = base_path::resolve_asset(anim_path, deps_.project_mgr->active());
    auto dir = fs::path(resolved).parent_path();

    std::string skel_rel;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() == ".fjskel") {
            skel_rel = fs::relative(entry.path(),
                base_path::resolve_asset("", deps_.project_mgr->active())).string();
            break;
        }
    }

    if (skel_rel.empty()) {
        FJELL_CORE_ERROR("AnimEditor: no .fjskel found for {}", anim_path);
        return;
    }

    open_skeleton(skel_rel);

    // Select and play the requested clip
    for (int i = 0; i < static_cast<int>(available_clip_paths_.size()); ++i) {
        if (available_clip_paths_[i] == anim_path) {
            load_clip(i);
            break;
        }
    }
}

void AnimationEditorContext::open_animset(const std::string& animset_path) {
    animset_path_ = animset_path;
    auto resolved = base_path::resolve_asset(animset_path, deps_.project_mgr->active());

    // Load the state machine
    if (auto r = state_machine_.load(resolved); !r) {
        FJELL_CORE_ERROR("AnimEditor: failed to load animset {}: {}", animset_path, r.error());
        animset_path_.clear();
        return;
    }

    name_ = fs::path(animset_path).stem().string();

    // Load skeleton from the path stored in the .fjanimset
    const auto& skel_path = state_machine_.skeleton_path();
    if (!skel_path.empty()) {
        open_skeleton(skel_path);
        load_animset_clips(resolved);
    }

    build_graph_from_state_machine();

    document_opened();
    FJELL_CORE_INFO("AnimEditor: opened animset {} ({} states, {} params)",
                    animset_path,
                    static_cast<int>(state_machine_.states().size()),
                    static_cast<int>(state_machine_.params_mutable().size()));
}

// ── Panels ───────────────────────────────────────────────────────────────

void AnimationEditorContext::add_bone_collider(CollisionPrimitiveKind kind,
                                              bool fit_to_bone) {
    if (!skeleton_ || selected_bone_ < 0) return;
    const auto& joints = skeleton_->joints();
    if (selected_bone_ >= static_cast<int>(joints.size())) return;

    BoneCollider col;
    col.bone_name = joints[static_cast<size_t>(selected_bone_)].name;
    col.shape.kind = kind;
    col.shape.bone = selected_bone_;

    if (fit_to_bone) {
        // A limb runs from its joint toward its child, so the distance between
        // them is the length worth covering. Without a child there is nothing
        // to measure against — a hand or a foot — so fall back to something
        // small rather than guessing a direction.
        float length = 0.0F;
        for (size_t i = 0; i < joints.size(); ++i) {
            if (joints[i].parent != selected_bone_) continue;
            length = std::max(length, glm::length(joints[i].translation));
        }

        if (length > 0.0F) {
            // Sit the shape at the midpoint so it spans joint to child.
            col.shape.position = {0.0F, length * 0.5F, 0.0F};
            float radius = std::max(length * 0.25F, 0.02F);
            switch (kind) {
                case CollisionPrimitiveKind::capsule:
                    col.shape.extents = {radius, length, 0.0F};
                    break;
                case CollisionPrimitiveKind::sphere:
                    col.shape.extents = glm::vec3{length * 0.5F};
                    break;
                default:
                    col.shape.extents = {radius, length * 0.5F, radius};
                    break;
            }
        } else {
            col.shape.extents = glm::vec3{0.05F};
        }
    }

    colliders_.push_back(col);
    selected_collider_ = static_cast<int>(colliders_.size()) - 1;
    colliders_dirty_ = true;
}

void AnimationEditorContext::draw_collider_panel() {
    if (ImGui::Begin(ctx_title("Bone Colliders").c_str())) {
        if (!skeleton_) {
            ImGui::TextDisabled("No skeleton loaded");
            ImGui::End();
            return;
        }

        if (ImGui::Checkbox("Show in preview", &show_colliders_)) {
            // Redrawn from the list each frame; nothing else to do.
        }

        const auto& joints = skeleton_->joints();
        bool has_bone = selected_bone_ >= 0 &&
                        selected_bone_ < static_cast<int>(joints.size());

        static constexpr struct { CollisionPrimitiveKind kind; const char* label; } KINDS[] = {
            {CollisionPrimitiveKind::capsule, "Capsule"},
            {CollisionPrimitiveKind::sphere, "Sphere"},
            {CollisionPrimitiveKind::box, "Box"},
        };

        ImGui::BeginDisabled(!has_bone);
        if (ImGui::Button("Fit to Bone")) ImGui::OpenPopup("fit_bone_kind");
        if (ImGui::BeginPopup("fit_bone_kind")) {
            for (const auto& k : KINDS) {
                if (ImGui::Selectable(k.label)) add_bone_collider(k.kind, true);
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Add")) ImGui::OpenPopup("add_bone_kind");
        if (ImGui::BeginPopup("add_bone_kind")) {
            for (const auto& k : KINDS) {
                if (ImGui::Selectable(k.label)) add_bone_collider(k.kind, false);
            }
            ImGui::EndPopup();
        }
        ImGui::EndDisabled();

        if (!has_bone) {
            ImGui::TextDisabled("Select a bone to add a shape to it.");
        } else {
            ImGui::SameLine();
            ImGui::TextDisabled("on %s", joints[static_cast<size_t>(selected_bone_)].name.c_str());
        }

        ImGui::Separator();

        if (colliders_.empty()) {
            ImGui::TextWrapped(
                "No shapes. A skeleton without them collides only as whatever "
                "collider its node carries — add shapes here to get per-bone "
                "hit detection.");
        }

        static const char* KIND_NAMES[] = {"Box", "Sphere", "Capsule",
                                            "Convex Hull", "Triangle Mesh"};

        for (int i = 0; i < static_cast<int>(colliders_.size()); ++i) {
            auto& col = colliders_[static_cast<size_t>(i)];
            ImGui::PushID(i);

            bool selected = selected_collider_ == i;
            auto label = std::string(KIND_NAMES[static_cast<int>(col.shape.kind)]) +
                         " — " + col.bone_name;
            if (ImGui::Selectable(label.c_str(), selected)) {
                selected_collider_ = selected ? -1 : i;
                // Selecting a shape follows it to its bone, so the tree and the
                // preview agree about what is being edited.
                if (!selected && col.shape.bone >= 0) selected_bone_ = col.shape.bone;
            }

            if (selected) {
                ImGui::Indent();
                ImGui::SetNextItemWidth(-1);
                if (ImGui::DragFloat3("##Pos", &col.shape.position.x, 0.005F)) {
                    colliders_dirty_ = true;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Position, in bone space");

                switch (col.shape.kind) {
                    case CollisionPrimitiveKind::capsule:
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::DragFloat("##Radius", &col.shape.extents.x, 0.005F,
                                             0.001F, 100.0F)) {
                            colliders_dirty_ = true;
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Radius");
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::DragFloat("##Height", &col.shape.extents.y, 0.005F,
                                             0.001F, 100.0F)) {
                            colliders_dirty_ = true;
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Height");
                        break;
                    case CollisionPrimitiveKind::sphere:
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::DragFloat("##SphRadius", &col.shape.extents.x, 0.005F,
                                             0.001F, 100.0F)) {
                            colliders_dirty_ = true;
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Radius");
                        break;
                    default:
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::DragFloat3("##Ext", &col.shape.extents.x, 0.005F,
                                              0.001F, 100.0F)) {
                            colliders_dirty_ = true;
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Half extents");
                        break;
                }

                if (ImGui::SmallButton("Remove")) {
                    colliders_.erase(colliders_.begin() + i);
                    selected_collider_ = -1;
                    colliders_dirty_ = true;
                    ImGui::Unindent();
                    ImGui::PopID();
                    break;
                }
                ImGui::Unindent();
            }
            ImGui::PopID();
        }
    }
    ImGui::End();
}

void AnimationEditorContext::draw_bone_tree() {
    if (ImGui::Begin(ctx_title("Bone Tree").c_str())) {
        if (!skeleton_) {
            ImGui::TextDisabled("No skeleton loaded");
        } else {
            const auto& joints = skeleton_->joints();

            // Build children list
            std::vector<std::vector<int>> children(joints.size());
            std::vector<int> roots;
            for (int i = 0; i < static_cast<int>(joints.size()); ++i) {
                if (joints[i].parent >= 0) {
                    children[joints[i].parent].push_back(i);
                } else {
                    roots.push_back(i);
                }
            }

            std::function<void(int, int)> draw_joint = [&](int idx, int depth) {
                const std::string id = std::to_string(idx);
                const auto row = ui::tree_row({
                    .id = id.c_str(),
                    .name = joints[idx].name,
                    .depth = depth,
                    .has_children = !children[idx].empty(),
                    .icon = ui::icon::skeleton,
                    .category = theme::Category::Animation,
                    .selection = idx == selected_bone_ ? ui::RowSelection::Primary : ui::RowSelection::None,
                });
                if (row.clicked) selected_bone_ = idx;
                if (row.open) {
                    for (int child : children[idx]) draw_joint(child, depth + 1);
                }
            };

            if (auto tree = ui::Tree("##bones")) {
                for (int root : roots) draw_joint(root, 0);
            }
        }
    }
    ImGui::End();
}

void AnimationEditorContext::draw_clip_list() {
    if (ImGui::Begin(ctx_title("Clip List").c_str())) {
        if (available_clip_names_.empty()) {
            ImGui::TextDisabled("No clips found");
        } else {
            for (int i = 0; i < static_cast<int>(available_clip_names_.size()); ++i) {
                ImGui::PushID(i);
                bool selected = (i == selected_clip_);
                if (ImGui::Selectable(available_clip_names_[i].c_str(), selected)) {
                    load_clip(i);
                }
                ImGui::PopID();
            }
        }
    }
    ImGui::End();
}

void AnimationEditorContext::draw_properties() {
    if (ImGui::Begin(ctx_title("Anim Properties").c_str())) {
        auto& states = state_machine_.states_mutable();

        // Skeleton assignment (always visible in animset mode)
        if (!animset_path_.empty()) {
            ImGui::SeparatorText("Skeleton");
            auto skel = state_machine_.skeleton_path();
            if (skel.empty()) {
                ImGui::TextDisabled("None — drag .fjskel here");
            } else {
                auto filename = fs::path(skel).filename().string();
                ImGui::Text("%s", filename.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("Clear##skel")) {
                    state_machine_.set_skeleton_path("");
                }
            }
            if (ImGui::BeginDragDropTarget()) {
                if (auto* payload = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                    std::string path(static_cast<const char*>(payload->Data),
                                     static_cast<size_t>(payload->DataSize - 1));
                    if (path.ends_with(".fjskel")) {
                        state_machine_.set_skeleton_path(path);
                        open_skeleton(path);
                        auto resolved = base_path::resolve_asset(animset_path_, deps_.project_mgr->active());
                        load_animset_clips(resolved);
                    }
                }
                ImGui::EndDragDropTarget();
            }
            ImGui::Separator();
        }

        // State node selected in graph
        if (selected_state_node_ >= 0 &&
            selected_state_node_ < static_cast<int>(state_nodes_.size())) {
            int si = state_nodes_[selected_state_node_].state_index;
            if (si >= 0 && si < static_cast<int>(states.size())) {
                auto& state = states[si];

                ImGui::SeparatorText("State");

                // Name
                char name_buf[128];
                std::snprintf(name_buf, sizeof(name_buf), "%s", state.name.c_str());
                if (ImGui::InputText("Name", name_buf, sizeof(name_buf),
                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
                    std::string old_name = state.name;
                    state.name = name_buf;
                    if (state_machine_.initial_state_name() == old_name) {
                        state_machine_.set_initial(state.name);
                    }
                    state_machine_.rebuild_index();
                    // Update transition targets referencing old name
                    for (auto& s : states) {
                        for (auto& t : s.transitions) {
                            if (t.target == old_name) t.target = state.name;
                        }
                    }
                }

                // Clip (drag-drop)
                char clip_buf[256];
                std::snprintf(clip_buf, sizeof(clip_buf), "%s", state.clip.c_str());
                ImGui::InputText("Clip", clip_buf, sizeof(clip_buf));
                if (ImGui::BeginDragDropTarget()) {
                    if (auto* payload = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                        state.clip = std::string(static_cast<const char*>(payload->Data),
                                                 static_cast<size_t>(payload->DataSize - 1));
                        state.blend_space.clear();
                    }
                    ImGui::EndDragDropTarget();
                }

                // Blend space (drag-drop)
                char bs_buf[256];
                std::snprintf(bs_buf, sizeof(bs_buf), "%s", state.blend_space.c_str());
                ImGui::InputText("Blend Space", bs_buf, sizeof(bs_buf));
                if (ImGui::BeginDragDropTarget()) {
                    if (auto* payload = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                        state.blend_space = std::string(static_cast<const char*>(payload->Data),
                                                        static_cast<size_t>(payload->DataSize - 1));
                        state.clip.clear();
                    }
                    ImGui::EndDragDropTarget();
                }

                ImGui::DragFloat("Speed", &state.speed, 0.01f, 0.0f, 10.0f);
                ImGui::Checkbox("Loop", &state.looping);

                bool is_initial = (state.name == state_machine_.initial_state_name());
                if (!is_initial && ImGui::Button("Set as Initial")) {
                    state_machine_.set_initial(state.name);
                }
            }
        }
        // Transition link selected
        else if (selected_link_ >= 0 &&
                 selected_link_ < static_cast<int>(transition_links_.size())) {
            auto& link = transition_links_[selected_link_];
            int si = link.source_state;
            int ti = link.transition_index;
            if (si >= 0 && si < static_cast<int>(states.size()) &&
                ti >= 0 && ti < static_cast<int>(states[si].transitions.size())) {
                auto& trans = states[si].transitions[ti];

                ImGui::SeparatorText("Transition");
                ImGui::Text("From: %s", states[si].name.c_str());
                ImGui::Text("To: %s", trans.target.c_str());
                ImGui::DragFloat("Blend Time", &trans.blend_time, 0.01f, 0.0f, 5.0f);

                ImGui::Separator();
                ImGui::Text("Conditions:");

                // Collect param names for combo
                auto& params = state_machine_.params_mutable();
                std::vector<std::string> param_names;
                for (const auto& [name, _] : params) {
                    param_names.push_back(name);
                }
                std::sort(param_names.begin(), param_names.end());

                static const char* op_names[] = {"==", "!=", "<", "<=", ">", ">="};

                int remove_idx = -1;
                for (int ci = 0; ci < static_cast<int>(trans.conditions.size()); ++ci) {
                    auto& cond = trans.conditions[ci];
                    ImGui::PushID(ci);

                    // Param combo
                    ImGui::SetNextItemWidth(100.0f);
                    if (ImGui::BeginCombo("##param", cond.param.c_str())) {
                        for (const auto& pn : param_names) {
                            if (ImGui::Selectable(pn.c_str(), pn == cond.param)) {
                                cond.param = pn;
                            }
                        }
                        ImGui::EndCombo();
                    }

                    // Op combo
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(50.0f);
                    int op_idx = static_cast<int>(cond.op);
                    if (ImGui::Combo("##op", &op_idx, op_names, 6)) {
                        cond.op = static_cast<CompareOp>(op_idx);
                    }

                    // Value
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(80.0f);
                    ImGui::DragFloat("##val", &cond.value, 0.01f);

                    // Remove
                    ImGui::SameLine();
                    if (ImGui::SmallButton("X")) {
                        remove_idx = ci;
                    }

                    ImGui::PopID();
                }

                if (remove_idx >= 0) {
                    trans.conditions.erase(trans.conditions.begin() + remove_idx);
                }

                if (ImGui::Button("Add Condition")) {
                    TransitionCondition cond;
                    if (!param_names.empty()) cond.param = param_names[0];
                    trans.conditions.push_back(std::move(cond));
                }
            }
        }
        // Bone selected
        else if (skeleton_ && selected_bone_ >= 0 &&
                 selected_bone_ < static_cast<int>(skeleton_->joints().size())) {
            const auto& joint = skeleton_->joints()[selected_bone_];
            ImGui::Text("Bone: %s", joint.name.c_str());
            ImGui::Text("Index: %d", selected_bone_);
            ImGui::Text("Parent: %d", joint.parent);
            ImGui::Separator();
            ImGui::Text("Rest Pose:");
            ImGui::Text("  T: %.3f, %.3f, %.3f",
                joint.translation.x, joint.translation.y, joint.translation.z);
            ImGui::Text("  R: %.3f, %.3f, %.3f, %.3f",
                joint.rotation.x, joint.rotation.y, joint.rotation.z, joint.rotation.w);
            ImGui::Text("  S: %.3f, %.3f, %.3f",
                joint.scale.x, joint.scale.y, joint.scale.z);
        } else if (current_clip_) {
            ImGui::Text("Clip: %s", current_clip_->name().c_str());
            ImGui::Text("Duration: %.2fs", current_clip_->duration());
            ImGui::Text("Tracks: %d", static_cast<int>(current_clip_->tracks().size()));
        } else {
            ImGui::TextDisabled("No selection");
        }

        // Notify editing (when a clip is loaded and a notify marker is selected)
        if (current_clip_ && selected_event_index_ >= 0 &&
            selected_event_index_ < static_cast<int>(current_clip_->notifies().size())) {
            auto& ni = current_clip_->notifies()[selected_event_index_];

            ImGui::Separator();
            ImGui::SeparatorText("Notify");

            // Type
            ImGui::TextDisabled("Type: %s", ni.type_name.c_str());

            // Time
            float t = ni.time;
            if (ImGui::DragFloat("Time", &t, 0.001f, 0.0f, current_clip_->duration(), "%.3fs")) {
                ni.time = t;
                sync_markers_from_clip();
            }

            // End time (for state notifies)
            if (ni.end_time > 0.0f || (ni.instance && ni.instance->is_state())) {
                float et = ni.end_time;
                if (ImGui::DragFloat("End Time", &et, 0.001f, ni.time, current_clip_->duration(), "%.3fs")) {
                    ni.end_time = et;
                    sync_markers_from_clip();
                }
            }

            // Reflected fields (if instance exists and type is registered)
            if (ni.instance) {
                const auto* info = AnimNotifyRegistry::instance().find(ni.type_name);
                if (info && !info->fields.empty()) {
                    ImGui::SeparatorText("Properties");
                    InspectorContext ictx{};
                    ictx.resource_manager = deps_.resource_manager;
                    ictx.project_mgr = deps_.project_mgr;
                    ictx.icons = deps_.icon_cache;
                    (void)draw_script_fields(info->fields, ni.instance.get(), ictx, nullptr);
                }
            }

            // Delete notify button
            ImGui::Spacing();
            if (ImGui::Button("Delete Notify")) {
                current_clip_->notifies().erase(
                    current_clip_->notifies().begin() + selected_event_index_);
                selected_event_index_ = -1;
                sync_markers_from_clip();
            }
        }
    }
    ImGui::End();
}

// ── Internal ─────────────────────────────────────────────────────────────

void AnimationEditorContext::scan_clips() {
    available_clip_paths_.clear();
    available_clip_names_.clear();

    if (skeleton_path_.empty()) return;

    auto resolved = base_path::resolve_asset(skeleton_path_, deps_.project_mgr->active());
    auto dir = fs::path(resolved).parent_path();
    auto base = base_path::resolve_asset("", deps_.project_mgr->active());

    if (!fs::is_directory(dir)) return;

    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() == ".fjanim") {
            auto rel = fs::relative(entry.path(), base).string();
            available_clip_paths_.push_back(rel);
            available_clip_names_.push_back(entry.path().stem().string());
        }
    }

    // Sort names and paths together so indices stay in sync
    std::vector<std::pair<std::string, std::string>> pairs;
    for (size_t i = 0; i < available_clip_names_.size(); ++i) {
        pairs.emplace_back(available_clip_names_[i], available_clip_paths_[i]);
    }
    std::sort(pairs.begin(), pairs.end());
    for (size_t i = 0; i < pairs.size(); ++i) {
        available_clip_names_[i] = pairs[i].first;
        available_clip_paths_[i] = pairs[i].second;
    }
}

void AnimationEditorContext::load_clip(int index) {
    if (index < 0 || index >= static_cast<int>(available_clip_paths_.size())) return;

    selected_clip_ = index;
    current_clip_path_ = available_clip_paths_[index];
    auto resolved = base_path::resolve_asset(current_clip_path_, deps_.project_mgr->active());

    auto clip_result = AnimationClip::load(resolved);
    if (!clip_result) {
        FJELL_CORE_ERROR("AnimEditor: failed to load clip: {}", clip_result.error());
        return;
    }
    current_clip_ = std::make_shared<AnimationClip>(std::move(*clip_result));

    // Update timeline
    timeline_.set_duration(current_clip_->duration());
    timeline_.set_time(0.0f);
    timeline_.set_playing(true);

    // Sync event markers to timeline
    sync_markers_from_clip();
    selected_event_index_ = -1;

    // The new clip's notifies are as its file holds them; the colliders and
    // the animset keep whatever edits they have.
    if (auto saved = nlohmann::json::parse(saved_document(), nullptr, false); !saved.is_discarded()) {
        saved["notifies"] = current_clip_->notifies_text();
        set_saved_document(saved.dump());
    } else {
        document_opened();
    }

    // Play on the preview animator
    auto* node = preview_scene_.find(preview_node_id_);
    if (!node) return;
    auto* anim = node->components.get<AnimatorComponent>();
    if (!anim) return;

    anim->play(current_clip_path_, current_clip_.get());
}

void AnimationEditorContext::sync_markers_from_clip() {
    std::vector<TimelineMarker> markers;
    if (current_clip_) {
        for (const auto& ni : current_clip_->notifies()) {
            markers.push_back({ni.time, ni.type_name});
        }
    }
    timeline_.set_markers(markers);
}

void AnimationEditorContext::load_animset_clips(const std::string& animset_resolved_path) {
    if (preview_node_id_ == 0) return;

    auto* node = preview_scene_.find(preview_node_id_);
    if (!node) return;
    auto* anim = node->components.get<AnimatorComponent>();
    if (!anim) return;

    for (const auto& state : state_machine_.states()) {
        if (state.clip.empty()) continue;
        auto clip_resolved = base_path::resolve_asset(state.clip, deps_.project_mgr->active());
        auto clip_result = AnimationClip::load(clip_resolved);
        if (clip_result) {
            auto clip = std::make_shared<AnimationClip>(std::move(*clip_result));
            anim->set_clip(state.clip, clip.get());
            anim->clips.push_back(std::move(clip));
            anim->clip_paths.push_back(state.clip);
        } else {
            FJELL_CORE_WARN("AnimEditor: failed to load clip {}: {}", state.clip, clip_result.error());
        }
    }

    // Set up state machine on the preview animator
    if (auto r = anim->load_state_machine(animset_resolved_path); !r) {
        FJELL_CORE_WARN("AnimEditor: failed to load state machine: {}", r.error());
    }

    // Resolve blend spaces referenced by states
    std::string base_dir = deps_.project_mgr->active() ? deps_.project_mgr->active()->root().string() : "";
    anim->resolve_blend_spaces(base_dir);
}

void AnimationEditorContext::update_preview(float dt) {
    if (preview_node_id_ == 0) return;

    auto* node = preview_scene_.find(preview_node_id_);
    if (!node) return;
    auto* anim = node->components.get<AnimatorComponent>();
    if (!anim) return;

    if (!animset_path_.empty()) {
        // State machine mode: forward params to preview animator
        for (const auto& [name, value] : state_machine_.params_mutable()) {
            anim->set_param(name, value);
        }
    } else {
        // Single clip mode: sync animator time with timeline
        if (timeline_.playing()) {
            anim->set_time(timeline_.time());
        }
    }

    // Tick animation + world matrices on the preview scene
    preview_scene_.compute_world_matrices(nullptr);
    auto& system = deps_.animation_subsystem->system();
    system.update(preview_scene_, dt);
}

// ── State graph ─────────────────────────────────────────────────────────

void AnimationEditorContext::build_graph_from_state_machine() {
    state_nodes_.clear();
    transition_links_.clear();
    pending_positions_.clear();
    node_id_counter_ = 0;
    selected_state_node_ = -1;
    selected_link_ = -1;

    const auto& states = state_machine_.states();
    constexpr float x_spacing = 250.0f;
    constexpr float y_spacing = 200.0f;
    constexpr int cols = 4;

    for (int i = 0; i < static_cast<int>(states.size()); ++i) {
        AnimStateNode sn;
        sn.node_id = next_node_id();
        sn.input_pin_id = next_node_id();
        sn.output_pin_id = next_node_id();
        sn.state_index = i;
        pending_positions_[sn.node_id] = ImVec2(
            50.0f + static_cast<float>(i % cols) * x_spacing,
            50.0f + static_cast<float>(i / cols) * y_spacing);
        state_nodes_.push_back(sn);
    }

    // Build transition links
    for (int si = 0; si < static_cast<int>(states.size()); ++si) {
        for (int ti = 0; ti < static_cast<int>(states[si].transitions.size()); ++ti) {
            const auto& trans = states[si].transitions[ti];
            // Find target node
            int target_idx = -1;
            for (int j = 0; j < static_cast<int>(states.size()); ++j) {
                if (states[j].name == trans.target) {
                    target_idx = j;
                    break;
                }
            }
            if (target_idx < 0) continue;

            AnimTransitionLink link;
            link.link_id = next_node_id();
            link.start_pin = state_nodes_[si].output_pin_id;
            link.end_pin = state_nodes_[target_idx].input_pin_id;
            link.source_state = si;
            link.target_state = target_idx;
            link.transition_index = ti;
            transition_links_.push_back(link);
        }
    }
}

void AnimationEditorContext::draw_state_graph() {
    if (ImGui::Begin(ctx_title("State Graph").c_str())) {
        if (state_machine_.states().empty() && animset_path_.empty()) {
            ImGui::TextDisabled("No state machine loaded");
            ImGui::End();
            return;
        }

        // Context menu (right-click on empty space)
        if (ImGui::BeginPopupContextWindow("##state_graph_ctx")) {
            if (ImGui::MenuItem("Add State")) {
                AnimationState new_state;
                new_state.name = "State_" + std::to_string(state_machine_.states().size());
                state_machine_.add_state(std::move(new_state));
                build_graph_from_state_machine();
            }
            ImGui::EndPopup();
        }

        // Save/restore imnodes context
        auto* prev_ctx = ImNodes::GetCurrentContext();
        ImNodes::SetCurrentContext(static_cast<ImNodesContext*>(imnodes_ctx_));

        ImNodes::BeginNodeEditor();

        // Apply deferred positions
        for (const auto& [node_id, pos] : pending_positions_) {
            ImNodes::SetNodeEditorSpacePos(node_id, pos);
        }
        pending_positions_.clear();

        const auto& states = state_machine_.states();
        const auto& initial = state_machine_.initial_state_name();

        // Draw state nodes
        for (int i = 0; i < static_cast<int>(state_nodes_.size()); ++i) {
            const auto& sn = state_nodes_[i];
            if (sn.state_index >= static_cast<int>(states.size())) continue;
            const auto& state = states[sn.state_index];

            // Color title bar based on state type
            bool pushed_color = false;
            if (state.name == initial) {
                // Initial state: green
                ImNodes::PushColorStyle(ImNodesCol_TitleBar, IM_COL32(50, 140, 70, 255));
                ImNodes::PushColorStyle(ImNodesCol_TitleBarHovered, IM_COL32(60, 160, 80, 255));
                ImNodes::PushColorStyle(ImNodesCol_TitleBarSelected, IM_COL32(70, 180, 90, 255));
                pushed_color = true;
            } else if (state.uses_blend_space()) {
                // Blend space state: orange
                ImNodes::PushColorStyle(ImNodesCol_TitleBar, IM_COL32(180, 120, 40, 255));
                ImNodes::PushColorStyle(ImNodesCol_TitleBarHovered, IM_COL32(200, 140, 50, 255));
                ImNodes::PushColorStyle(ImNodesCol_TitleBarSelected, IM_COL32(220, 160, 60, 255));
                pushed_color = true;
            }

            ImNodes::BeginNode(sn.node_id);

            ImNodes::BeginNodeTitleBar();
            ImGui::TextUnformatted(state.name.c_str());
            ImNodes::EndNodeTitleBar();

            // Node body
            if (!state.clip.empty()) {
                auto filename = fs::path(state.clip).stem().string();
                ImGui::TextDisabled("%s", filename.c_str());
            } else if (!state.blend_space.empty()) {
                auto filename = fs::path(state.blend_space).stem().string();
                ImGui::TextDisabled("BS: %s", filename.c_str());
            }

            if (state.speed != 1.0f) {
                ImGui::TextDisabled("%.1fx", state.speed);
            }
            if (state.looping) {
                ImGui::SameLine();
                ImGui::TextDisabled("loop");
            }

            // Input pin
            ImNodes::BeginInputAttribute(sn.input_pin_id);
            ImGui::TextUnformatted("In");
            ImNodes::EndInputAttribute();

            // Output pin
            ImNodes::BeginOutputAttribute(sn.output_pin_id);
            ImGui::Indent(80.0f);
            ImGui::TextUnformatted("Out");
            ImNodes::EndOutputAttribute();

            ImNodes::EndNode();

            if (pushed_color) {
                ImNodes::PopColorStyle();
                ImNodes::PopColorStyle();
                ImNodes::PopColorStyle();
            }
        }

        // Draw transition links
        for (const auto& link : transition_links_) {
            ImNodes::Link(link.link_id, link.start_pin, link.end_pin);
        }

        ImNodes::EndNodeEditor();

        // Handle new link creation
        int start_pin = 0, end_pin = 0;
        if (ImNodes::IsLinkCreated(&start_pin, &end_pin)) {
            // Find source and target state nodes by pin IDs
            int source_idx = -1, target_idx = -1;
            for (int i = 0; i < static_cast<int>(state_nodes_.size()); ++i) {
                if (state_nodes_[i].output_pin_id == start_pin) source_idx = i;
                if (state_nodes_[i].input_pin_id == end_pin) target_idx = i;
            }
            if (source_idx >= 0 && target_idx >= 0) {
                int si = state_nodes_[source_idx].state_index;
                int ti = state_nodes_[target_idx].state_index;
                auto& source_state = state_machine_.states_mutable()[si];

                StateTransition new_trans;
                new_trans.target = state_machine_.states()[ti].name;
                int trans_idx = static_cast<int>(source_state.transitions.size());
                source_state.transitions.push_back(std::move(new_trans));

                AnimTransitionLink link;
                link.link_id = next_node_id();
                link.start_pin = start_pin;
                link.end_pin = end_pin;
                link.source_state = si;
                link.target_state = ti;
                link.transition_index = trans_idx;
                transition_links_.push_back(link);
            }
        }

        // Handle selection
        selected_state_node_ = -1;
        selected_link_ = -1;

        if (ImNodes::NumSelectedNodes() > 0) {
            int sel_id = 0;
            ImNodes::GetSelectedNodes(&sel_id);
            for (int i = 0; i < static_cast<int>(state_nodes_.size()); ++i) {
                if (state_nodes_[i].node_id == sel_id) {
                    selected_state_node_ = i;
                    break;
                }
            }
        }

        if (ImNodes::NumSelectedLinks() > 0) {
            int sel_id = 0;
            ImNodes::GetSelectedLinks(&sel_id);
            for (int i = 0; i < static_cast<int>(transition_links_.size()); ++i) {
                if (transition_links_[i].link_id == sel_id) {
                    selected_link_ = i;
                    break;
                }
            }
        }

        // Handle Delete key
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
            bool needs_rebuild = false;

            // Delete selected links (transitions)
            int num_sel_links = ImNodes::NumSelectedLinks();
            if (num_sel_links > 0) {
                std::vector<int> sel_ids(static_cast<size_t>(num_sel_links));
                ImNodes::GetSelectedLinks(sel_ids.data());
                for (int link_id : sel_ids) {
                    for (auto it = transition_links_.begin(); it != transition_links_.end(); ++it) {
                        if (it->link_id == link_id) {
                            int si = it->source_state;
                            int ti = it->transition_index;
                            auto& trans_vec = state_machine_.states_mutable()[si].transitions;
                            if (ti < static_cast<int>(trans_vec.size())) {
                                trans_vec.erase(trans_vec.begin() + ti);
                            }
                            needs_rebuild = true;
                            break;
                        }
                    }
                }
            }

            // Delete selected nodes (states)
            int num_sel_nodes = ImNodes::NumSelectedNodes();
            if (num_sel_nodes > 0) {
                std::vector<int> sel_ids(static_cast<size_t>(num_sel_nodes));
                ImNodes::GetSelectedNodes(sel_ids.data());
                for (int node_id : sel_ids) {
                    for (const auto& sn : state_nodes_) {
                        if (sn.node_id == node_id) {
                            const auto& name = state_machine_.states()[sn.state_index].name;
                            state_machine_.remove_state(name);
                            needs_rebuild = true;
                            break;
                        }
                    }
                }
            }

            if (needs_rebuild) {
                build_graph_from_state_machine();
            }
        }

        ImNodes::SetCurrentContext(prev_ctx);
    }
    ImGui::End();
}

// ── Parameters ──────────────────────────────────────────────────────────

void AnimationEditorContext::draw_parameters() {
    if (ImGui::Begin(ctx_title("Anim Parameters").c_str())) {
        if (animset_path_.empty()) {
            ImGui::TextDisabled("No state machine loaded");
            ImGui::End();
            return;
        }

        auto& params = state_machine_.params_mutable();

        // Add parameter
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputText("##new_param", new_param_name_, sizeof(new_param_name_));
        ImGui::SameLine();
        if (ImGui::SmallButton("Add") && new_param_name_[0] != '\0') {
            std::string name(new_param_name_);
            if (params.find(name) == params.end()) {
                params[name] = 0.0f;
                new_param_name_[0] = '\0';
            }
        }

        ImGui::Separator();

        // Sort param names for stable display
        std::vector<std::string> names;
        names.reserve(params.size());
        for (const auto& [name, _] : params) {
            names.push_back(name);
        }
        std::sort(names.begin(), names.end());

        std::string remove_name;
        for (const auto& name : names) {
            ImGui::PushID(name.c_str());
            float& val = params[name];
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 30.0f);
            ImGui::SliderFloat(name.c_str(), &val, -10.0f, 10.0f);
            ImGui::SameLine();
            if (ImGui::SmallButton("X")) {
                remove_name = name;
            }
            ImGui::PopID();
        }

        if (!remove_name.empty()) {
            params.erase(remove_name);
        }

        // State machine status
        if (!state_machine_.states().empty()) {
            ImGui::Separator();
            ImGui::TextDisabled("State: %s", state_machine_.current_state_name().c_str());
            if (state_machine_.is_transitioning()) {
                ImGui::TextDisabled("-> %s (%.0f%%)",
                    state_machine_.next_state() ? state_machine_.next_state()->name.c_str() : "?",
                    state_machine_.crossfade_alpha() * 100.0f);
            }
        }
    }
    ImGui::End();
}

// ── Sync preview ────────────────────────────────────────────────────────

void AnimationEditorContext::sync_preview_state_machine() {
    if (preview_node_id_ == 0 || animset_path_.empty()) return;

    auto* node = preview_scene_.find(preview_node_id_);
    if (!node) return;
    auto* anim = node->components.get<AnimatorComponent>();
    if (!anim) return;

    // Save to temp, reload into preview animator
    auto resolved = base_path::resolve_asset(animset_path_, deps_.project_mgr->active());
    if (auto r = state_machine_.save(resolved); !r) {
        FJELL_CORE_ERROR("AnimEditor: failed to save animset for preview: {}", r.error());
        return;
    }
    if (auto r = anim->load_state_machine(resolved); !r) {
        FJELL_CORE_ERROR("AnimEditor: failed to load state machine for preview: {}", r.error());
        return;
    }

    // Re-wire clips
    for (const auto& state : state_machine_.states()) {
        if (state.clip.empty()) continue;
        auto clip_resolved = base_path::resolve_asset(state.clip, deps_.project_mgr->active());
        auto clip_result = AnimationClip::load(clip_resolved);
        if (clip_result) {
            auto clip = std::make_shared<AnimationClip>(std::move(*clip_result));
            anim->set_clip(state.clip, clip.get());
            anim->clips.push_back(std::move(clip));
        } else {
            FJELL_CORE_WARN("Failed to load clip {}: {}", state.clip, clip_result.error());
        }
    }

    std::string base_dir = deps_.project_mgr->active() ? deps_.project_mgr->active()->root().string() : "";
    anim->resolve_blend_spaces(base_dir);
}

} // namespace fjell
