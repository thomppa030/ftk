#include "ui/animation_editor_context.hpp"
#include "ui/inspectors/collision_shape_fields.hpp"
#include "ui/kit/row.hpp"
#include "ui/kit/button.hpp"
#include "ui/kit/choice.hpp"
#include "ui/kit/dialog.hpp"
#include "ui/kit/menu.hpp"
#include "ui/kit/section.hpp"
#include "ui/kit/text_field.hpp"
#include "ui/asset_ref_widget.hpp"
#include "ui/kit/list_editor.hpp"
#include "ui/kit/field.hpp"
#include "ui/kit/feedback.hpp"
#include "ui/panel_widget.hpp"
#include "animation/anim_notify_registry.hpp"
#include "animation/animation_clip.hpp"
#include "animation/animation_subsystem.hpp"
#include "animation/animator_component.hpp"
#include "scene/component_meta.hpp"
#include "ui/inspectors/script_fields.hpp"
#include "ui/kit/icons.hpp"
#include "ui/kit/tree.hpp"
#include "ui/state_graph.hpp"
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

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace fjell {

namespace fs = std::filesystem;

AnimationEditorContext::AnimationEditorContext(AnimationEditorDeps deps)
    : deps_{deps} {}

EditorContext::DockPreset AnimationEditorContext::dock_preset() const {
    return {.tree = {"Bones", "Clips"}, .preview = {"Preview"}, .under_preview = {"State graph", "Timeline", "Parameters"}, .inspector = {"Properties", "Colliders"}};
}

void AnimationEditorContext::init() {
    viewport_manager_.init(deps_.vulkan_context->device_ref(),
                           deps_.vulkan_context->allocator(),
                           deps_.vulkan_context->msaa_samples());
    viewport_manager_.add_viewport(ViewportSource::final_color, "Preview");

    timeline_.set_title("Timeline");


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

    name_preview(viewport_manager_);
    timeline_.set_title(ctx_title("Timeline"));

    viewport_manager_.draw(*deps_.vulkan_context, *deps_.input, dt);
    timeline_.draw(dt);

    // Add Notify popup (opened by right-click on timeline)
    if (open_add_notify_popup_) {
        ImGui::OpenPopup("##AddNotify");
        open_add_notify_popup_ = false;
    }
    if (ImGui::BeginPopup("##AddNotify")) {
        ui::menu_heading("Add notify");
        AnimNotifyRegistry::instance().for_each([&](const std::string& name, const NotifyTypeInfo& info) {
            // A state notify lasts; the rest fire once.
            const std::string label = info.is_state ? name + " (lasts)" : name;
            if (!ui::menu_item({.label = label.c_str()}) || !current_clip_) return;
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

std::string AnimationEditorContext::saved_files() const {
    std::vector<std::string> files;
    if (!animset_path_.empty()) files.push_back(fs::path(animset_path_).filename().string());
    if (current_clip_ && !current_clip_path_.empty()) {
        files.push_back(fs::path(current_clip_path_).filename().string() + " (notifies)");
    }
    if (!skeleton_path_.empty()) files.push_back(fs::path(skeleton_path_).filename().string() + " (colliders)");
    std::string text;
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (i > 0) text += i + 1 == files.size() ? " and " : ", ";
        text += files[i];
    }
    return text;
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
        state_graph_.clear_selection();
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
    viewport_manager_.frame({0.0f, 1.0f, 0.0f}, 2.5f);

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

    state_graph_.show(animset_path);

    document_opened();
    FJELL_CORE_INFO("AnimEditor: opened animset {} ({} states, {} params)",
                    animset_path,
                    static_cast<int>(state_machine_.states().size()),
                    static_cast<int>(state_machine_.params_mutable().size()));
}

bool AnimationEditorContext::has_clip(const std::string& anim_path) const {
    return std::find(available_clip_paths_.begin(), available_clip_paths_.end(), anim_path) !=
           available_clip_paths_.end();
}

void AnimationEditorContext::select_clip(const std::string& anim_path) {
    const auto it = std::find(available_clip_paths_.begin(), available_clip_paths_.end(), anim_path);
    if (it != available_clip_paths_.end()) {
        load_clip(static_cast<int>(it - available_clip_paths_.begin()));
    }
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
    auto panel = Panel(ui::icon::collision, ctx_title("Colliders").c_str());
    if (!panel) return;
    if (!skeleton_) {
        ui::empty_state(ui::icon::skeleton, "No skeleton open", "Open a skeleton or an animset");
        return;
    }

    const auto& joints = skeleton_->joints();
    const bool has_bone = selected_bone_ >= 0 && selected_bone_ < static_cast<int>(joints.size());
    if (auto t = ui::PropertyTable("##collider_view")) {
        // Redrawn from the list each frame; nothing else to do.
        ui::row("Show in preview", [&] { (void)ui::checkbox("##show", show_colliders_); });
    }

    auto list = ui::ListEditor("##bone_colliders", colliders_.size(), {.reorder = false, .opens = true});
    if (colliders_.empty()) {
        list.empty("No shapes: the skeleton collides only as its node's collider. Add shapes for "
                   "hits per bone");
    }
    std::optional<std::size_t> removed;
    for (std::size_t i = 0; i < colliders_.size(); ++i) {
        auto& col = colliders_[i];
        const bool selected = selected_collider_ == static_cast<int>(i);
        if (!list.begin_item(i, selected)) continue;
        // Only the selected shape shows its fields; the others one line each.
        if (selected) {
            ImGui::Text("%s on %s", collision_kind_name(col.shape.kind), col.bone_name.c_str());
            if (auto t = ui::PropertyTable("##shape")) {
                if (collision_shape_rows(col.shape, "Relative to the bone").changed) colliders_dirty_ = true;
            }
        } else {
            ImGui::Text("%s \xc2\xb7 %s", col.bone_name.c_str(), collision_shape_summary(col.shape).c_str());
        }
        if (list.end_item()) removed = i;
        if (list.clicked()) {
            selected_collider_ = selected ? -1 : static_cast<int>(i);
            // Selecting a shape follows it to its bone, so the tree and the
            // preview agree about what is being edited.
            if (!selected && col.shape.bone >= 0) selected_bone_ = col.shape.bone;
        }
    }
    ImGui::BeginDisabled(!has_bone);
    if (list.add_button("Add shape")) ImGui::OpenPopup("##add_shape");
    ImGui::EndDisabled();
    if (!has_bone) {
        ui::hint("Select a bone to add a shape to it");
    }
    if (auto pick = add_shape_menu("##add_shape", "the bone")) add_bone_collider(pick->kind, pick->fitted);
    if (removed) {
        colliders_.erase(colliders_.begin() + static_cast<std::ptrdiff_t>(*removed));
        selected_collider_ = -1;
        colliders_dirty_ = true;
    }
}

void AnimationEditorContext::draw_bone_tree() {
    auto panel = Panel(ui::icon::skeleton, ctx_title("Bones").c_str());
    if (!panel) return;
    if (!skeleton_) {
        ui::empty_state(ui::icon::skeleton, "No skeleton open", "Open a skeleton or an animset");
        return;
    }
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

void AnimationEditorContext::draw_clip_list() {
    auto panel = Panel(ui::icon::animation, ctx_title("Clips").c_str());
    if (!panel) return;
    if (available_clip_names_.empty()) {
        ui::empty_state(ui::icon::animation, "No clips", "Clips next to the skeleton or in the animset show here");
        return;
    }
    if (auto tree = ui::Tree("##clips")) {
        for (int i = 0; i < static_cast<int>(available_clip_names_.size()); ++i) {
            const std::string id = std::to_string(i);
            const auto row = ui::tree_row({
                .id = id.c_str(),
                .name = available_clip_names_[static_cast<size_t>(i)],
                .icon = ui::icon::animation,
                .category = theme::Category::Animation,
                .selection = i == selected_clip_ ? ui::RowSelection::Primary : ui::RowSelection::None,
            });
            if (row.clicked) load_clip(i);
        }
    }
}

void AnimationEditorContext::draw_properties() {
    auto panel = Panel(ui::icon::properties, ctx_title("Properties").c_str());
    if (!panel) return;
    auto& states = state_machine_.states_mutable();
    InspectorContext ictx{};
    ictx.resource_manager = deps_.resource_manager;
    ictx.project_mgr = deps_.project_mgr;
    ictx.icons = deps_.icon_cache;

    // The animset's skeleton, always there while an animset is open.
    if (!animset_path_.empty()) {
        ui::section("Animset", ui::icon::animation);
        if (auto t = ui::PropertyTable("##animset")) {
            ui::row("Skeleton", "The skeleton the animset's clips are made for.", [&] {
                static const std::string skel_exts[] = {".fjskel"};
                std::string skel = state_machine_.skeleton_path();
                if (asset_slot({.label = "Skeleton", .id = "##skeleton", .extensions = skel_exts, .ctx = &ictx}, skel)) {
                    state_machine_.set_skeleton_path(skel);
                    if (!skel.empty()) {
                        open_skeleton(skel);
                        auto resolved = base_path::resolve_asset(animset_path_, deps_.project_mgr->active());
                        load_animset_clips(resolved);
                    }
                }
            });
        }
    }

    if (const int si = state_graph_.selected_state(); si >= 0 && si < static_cast<int>(states.size())) {
        draw_state_properties(states[static_cast<size_t>(si)]);
    } else if (state_graph_.selected_transition()) {
        draw_transition_properties();
    } else if (skeleton_ && selected_bone_ >= 0 && selected_bone_ < static_cast<int>(skeleton_->joints().size())) {
        const auto& joints = skeleton_->joints();
        const auto& joint = joints[static_cast<size_t>(selected_bone_)];
        ui::section("Bone", ui::icon::skeleton);
        if (auto t = ui::PropertyTable("##bone")) {
            char text[96];
            ui::row("Name", [&] { ui::readout(joint.name.c_str()); });
            std::snprintf(text, sizeof(text), "%d", selected_bone_);
            ui::row("Index", [&] { ui::readout(text); });
            const char* parent = joint.parent >= 0 ? joints[static_cast<size_t>(joint.parent)].name.c_str() : "None, a root";
            ui::row("Parent", [&] { ui::readout(parent); });
            std::snprintf(text, sizeof(text), "%.3f, %.3f, %.3f m", joint.translation.x, joint.translation.y,
                          joint.translation.z);
            ui::row("Rest position", [&] { ui::readout(text); });
            std::snprintf(text, sizeof(text), "%.3f, %.3f, %.3f, %.3f", joint.rotation.x, joint.rotation.y,
                          joint.rotation.z, joint.rotation.w);
            ui::row("Rest rotation", "As a quaternion: x, y, z, w.", [&] { ui::readout(text); });
            std::snprintf(text, sizeof(text), "%.3f, %.3f, %.3f", joint.scale.x, joint.scale.y, joint.scale.z);
            ui::row("Rest scale", [&] { ui::readout(text); });
        }
    } else if (current_clip_) {
        ui::section("Clip", ui::icon::animation);
        if (auto t = ui::PropertyTable("##clip")) {
            char text[64];
            ui::row("Name", [&] { ui::readout(current_clip_->name().c_str()); });
            std::snprintf(text, sizeof(text), "%.2f s", current_clip_->duration());
            ui::row("Duration", [&] { ui::readout(text); });
            std::snprintf(text, sizeof(text), "%d", static_cast<int>(current_clip_->tracks().size()));
            ui::row("Tracks", "One per bone the clip moves.", [&] { ui::readout(text); });
        }
    } else if (animset_path_.empty()) {
        ui::empty_state(ui::icon::nothing_selected, "Nothing selected", "Select a bone, a clip or a state");
    }

    draw_notify_properties();
}

void AnimationEditorContext::draw_state_properties(AnimationState& state) {
    auto& states = state_machine_.states_mutable();
    InspectorContext ictx{};
    ictx.resource_manager = deps_.resource_manager;
    ictx.project_mgr = deps_.project_mgr;
    ictx.icons = deps_.icon_cache;

    ui::section("State", ui::icon::animation);
    if (auto t = ui::PropertyTable("##state")) {
        ui::row("Name", [&] {
            std::string name = state.name;
            if (!ui::text_field("##name", name).committed || name.empty() || name == state.name) return;
            const std::string old_name = state.name;
            state.name = name;
            if (state_machine_.initial_state_name() == old_name) state_machine_.set_initial(state.name);
            state_machine_.rebuild_index();
            // Transitions follow the state they point at.
            for (auto& s : states) {
                for (auto& transition : s.transitions) {
                    if (transition.target == old_name) transition.target = state.name;
                }
            }
        });
        // A state plays one clip or one blend space; picking one clears the other.
        ui::row("Clip", [&] {
            static const std::string clip_exts[] = {".fjanim"};
            if (asset_slot({.label = "Clip", .id = "##clip", .extensions = clip_exts, .ctx = &ictx}, state.clip) &&
                !state.clip.empty()) {
                state.blend_space.clear();
            }
        });
        ui::row("Blend space", [&] {
            static const std::string blend_exts[] = {".fjblend1D", ".fjblend2D"};
            if (asset_slot({.label = "Blend space", .id = "##blend_space", .extensions = blend_exts, .ctx = &ictx},
                           state.blend_space) &&
                !state.blend_space.empty()) {
                state.clip.clear();
            }
        });
        ui::row("Speed", [&] {
            (void)ui::drag("##speed", state.speed, {.speed = 0.01f, .min = 0.0f, .max = 10.0f, .unit = ui::Unit::Times});
        });
        ui::row("Loop", [&] { (void)ui::checkbox("##loop", state.looping); });
        const bool initial = state.name == state_machine_.initial_state_name();
        ui::row("Initial", "The state the machine starts in.", [&] {
            if (initial) {
                ui::readout("This state");
            } else if (ui::action(ui::icon::play, "Start here")) {
                state_machine_.set_initial(state.name);
            }
        });
    }
}

void AnimationEditorContext::draw_transition_properties() {
    auto& states = state_machine_.states_mutable();
    const auto [si, ti] = *state_graph_.selected_transition();
    if (si < 0 || si >= static_cast<int>(states.size()) || ti < 0 ||
        ti >= static_cast<int>(states[static_cast<size_t>(si)].transitions.size())) {
        return;
    }
    auto& source = states[static_cast<size_t>(si)];
    auto& trans = source.transitions[static_cast<size_t>(ti)];

    ui::section("Transition", ui::icon::animation);
    if (auto t = ui::PropertyTable("##transition")) {
        ui::row("From", [&] { ui::readout(source.name.c_str()); });
        ui::row("To", [&] { ui::readout(trans.target.c_str()); });
        ui::row("Blend time", [&] {
            (void)ui::drag("##blend_time", trans.blend_time,
                           {.speed = 0.01f, .min = 0.0f, .max = 5.0f, .unit = ui::Unit::Seconds});
        });
    }

    // The parameters a condition can test, by name.
    std::vector<std::string> param_names;
    for (const auto& [name, value] : state_machine_.params_mutable()) param_names.push_back(name);
    std::sort(param_names.begin(), param_names.end());
    static constexpr const char* OPS[] = {"==", "!=", "<", "<=", ">", ">="};

    ui::subheading("Conditions");
    std::optional<std::size_t> remove;
    bool add = false;
    {
        ui::ListEditor list("##conditions", trans.conditions.size(), {.reorder = false, .opens = true});
        for (std::size_t ci = 0; ci < trans.conditions.size(); ++ci) {
            auto& cond = trans.conditions[ci];
            const bool open = open_condition_ == static_cast<int>(ci);
            if (!list.begin_item(ci, open)) continue;
            if (open) {
                if (auto t = ui::PropertyTable("##condition")) {
                    ui::row("Parameter", [&] {
                        std::vector<const char*> names;
                        int index = -1;
                        for (std::size_t p = 0; p < param_names.size(); ++p) {
                            names.push_back(param_names[p].c_str());
                            if (param_names[p] == cond.param) index = static_cast<int>(p);
                        }
                        if (names.empty()) {
                            ui::readout("No parameters yet");
                            return;
                        }
                        if (index < 0) index = 0;
                        if (ui::choice("##param", index, names).changed) {
                            cond.param = param_names[static_cast<size_t>(index)];
                        }
                    });
                    ui::row("Is", [&] {
                        int op = static_cast<int>(cond.op);
                        if (ui::choice("##op", op, OPS).changed) cond.op = static_cast<CompareOp>(op);
                    });
                    ui::row("Value", [&] { (void)ui::drag("##value", cond.value, {.speed = 0.01f}); });
                }
            } else {
                char summary[160];
                std::snprintf(summary, sizeof(summary), "%s %s %.2f", cond.param.empty() ? "?" : cond.param.c_str(),
                              OPS[static_cast<int>(cond.op)], cond.value);
                ImGui::TextUnformatted(summary);
            }
            if (list.end_item()) remove = ci;
            if (list.clicked()) open_condition_ = open ? -1 : static_cast<int>(ci);
        }
        if (trans.conditions.empty()) list.empty("No conditions: the transition is taken as soon as it can be");
        add = list.add_button("Add condition");
    }
    if (add) {
        TransitionCondition cond;
        if (!param_names.empty()) cond.param = param_names.front();
        trans.conditions.push_back(std::move(cond));
        open_condition_ = static_cast<int>(trans.conditions.size()) - 1;
    }
    if (remove) {
        trans.conditions.erase(trans.conditions.begin() + static_cast<std::ptrdiff_t>(*remove));
        if (open_condition_ == static_cast<int>(*remove)) open_condition_ = -1;
        else if (open_condition_ > static_cast<int>(*remove)) --open_condition_;
    }
}

void AnimationEditorContext::draw_notify_properties() {
    if (!current_clip_ || selected_event_index_ < 0 ||
        selected_event_index_ >= static_cast<int>(current_clip_->notifies().size())) {
        return;
    }
    auto& ni = current_clip_->notifies()[static_cast<size_t>(selected_event_index_)];

    ui::section("Notify", ui::icon::animation);
    if (auto t = ui::PropertyTable("##notify")) {
        ui::row("Type", [&] { ui::readout(ni.type_name.c_str()); });
        ui::row("Time", [&] {
            if (ui::drag("##time", ni.time, {.speed = 0.001f, .min = 0.0f, .max = current_clip_->duration(),
                                             .unit = ui::Unit::Seconds, .format = "%.3f"}).changed) {
                sync_markers_from_clip();
            }
        });
        // A state notify lasts from its time to its end.
        if (ni.end_time > 0.0f || (ni.instance && ni.instance->is_state())) {
            ui::row("End time", [&] {
                if (ui::drag("##end_time", ni.end_time, {.speed = 0.001f, .min = ni.time, .max = current_clip_->duration(),
                                                         .unit = ui::Unit::Seconds, .format = "%.3f"}).changed) {
                    sync_markers_from_clip();
                }
            });
        }
        ui::row("", [&] {
            if (ui::action(ui::icon::remove, "Delete notify", ui::ButtonKind::GhostDanger)) {
                current_clip_->notifies().erase(current_clip_->notifies().begin() + selected_event_index_);
                selected_event_index_ = -1;
                sync_markers_from_clip();
            }
        });
    }
    if (selected_event_index_ < 0) return;

    // The notify type's own fields, when it has any.
    if (ni.instance) {
        const auto* info = AnimNotifyRegistry::instance().find(ni.type_name);
        if (info && !info->fields.empty()) {
            ui::subheading("Its fields");
            InspectorContext ictx{};
            ictx.resource_manager = deps_.resource_manager;
            ictx.project_mgr = deps_.project_mgr;
            ictx.icons = deps_.icon_cache;
            (void)draw_script_fields(info->fields, ni.instance.get(), ictx, nullptr);
        }
    }
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
    timeline_.show(current_clip_path_);
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

void AnimationEditorContext::draw_state_graph() {
    auto panel = Panel(ui::icon::animation, ctx_title("State graph").c_str());
    if (!panel) return;
    if (state_machine_.states().empty() && animset_path_.empty()) {
        ui::empty_state(ui::icon::animation, "No state machine", "Open an animset to edit its states");
        return;
    }
    (void)state_graph_.draw(state_machine_);
}

// ── Parameters ──────────────────────────────────────────────────────────

void AnimationEditorContext::draw_parameters() {
    auto panel = Panel(ui::icon::properties, ctx_title("Parameters").c_str());
    if (!panel) return;
    if (animset_path_.empty()) {
        ui::empty_state(ui::icon::animation, "No state machine", "Parameters belong to an animset");
        return;
    }
    auto& params = state_machine_.params_mutable();

    // Sorted by name for a stable list.
    std::vector<std::string> names;
    names.reserve(params.size());
    for (const auto& [name, value] : params) names.push_back(name);
    std::sort(names.begin(), names.end());

    std::optional<std::string> remove;
    bool add = false;
    {
        ui::ListEditor list("##params", names.size(), {.reorder = false});
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (!list.begin_item(i)) continue;
            if (auto t = ui::PropertyTable("##param")) {
                ui::row(names[i].c_str(), [&] { (void)ui::slider("##value", params[names[i]], -10.0f, 10.0f); });
            }
            if (list.end_item()) remove = names[i];
        }
        if (names.empty()) list.empty("No parameters: transitions test these, and the game sets them");
        add = list.add_button("Add parameter");
    }
    if (add) {
        param_prompt_.clear();
        ui::open_dialog("##add_param");
    }
    if (remove) params.erase(*remove);
    if (ui::prompt_dialog("##add_param", {.title = "New parameter", .hint = "Its name, e.g. speed", .confirm = "Add"},
                          param_prompt_) == ui::DialogAnswer::Confirm &&
        !params.contains(param_prompt_)) {
        params[param_prompt_] = 0.0f;
    }

    // Where the preview's machine is.
    if (!state_machine_.states().empty()) {
        ui::section("Now", ui::icon::play);
        if (auto t = ui::PropertyTable("##now")) {
            ui::row("State", [&] { ui::readout(state_machine_.current_state_name().c_str()); });
            if (state_machine_.is_transitioning()) {
                char text[160];
                std::snprintf(text, sizeof(text), "%s, %.0f%%",
                              state_machine_.next_state() ? state_machine_.next_state()->name.c_str() : "?",
                              state_machine_.crossfade_alpha() * 100.0f);
                ui::row("Blending to", [&] { ui::readout(text); });
            }
        }
    }
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
