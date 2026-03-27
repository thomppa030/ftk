#include "ui/animation_editor_context.hpp"
#include "animation/animation_clip.hpp"
#include "animation/animation_subsystem.hpp"
#include "animation/animator_component.hpp"
#include "animation/skeleton.hpp"
#include "core/base_path.hpp"
#include "core/clock.hpp"
#include "core/input.hpp"
#include "core/log.hpp"
#include "core/project_manager.hpp"
#include "renderer/resource_manager.hpp"
#include "renderer/skeletal_mesh_component.hpp"
#include "renderer/skeletal_model.hpp"
#include "renderer/vulkan_context.hpp"
#include "scene/scene.hpp"
#include "ui/icon_cache.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace fjell {

namespace fs = std::filesystem;

AnimationEditorContext::AnimationEditorContext(AnimationEditorDeps deps)
    : deps_{deps} {}

void AnimationEditorContext::init() {
    viewport_manager_.init(deps_.vulkan_context->device_ref(),
                           deps_.vulkan_context->msaa_samples());
    viewport_manager_.add_viewport(ViewportSource::final_color, "Anim Preview");

    timeline_.set_title("Anim Timeline");

    // Wire timeline callbacks
    connections_.push_back(timeline_.on_scrub.bind([this](float time) {
        // Scrub the animator to this time
        auto* node = preview_scene_.find(preview_node_id_);
        if (!node) return;
        auto* smc = node->components.get<SkeletalMeshComponent>();
        if (!smc || !smc->animator) return;
        smc->animator->set_time(time);
    }));

    connections_.push_back(timeline_.on_stop.bind([this]() {
        auto* node = preview_scene_.find(preview_node_id_);
        if (!node) return;
        auto* smc = node->components.get<SkeletalMeshComponent>();
        if (!smc || !smc->animator) return;
        smc->animator->set_time(0.0f);
    }));
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

    viewport_manager_.draw(*deps_.vulkan_context, *deps_.input, dt);
    timeline_.draw(dt);
    draw_bone_tree();
    draw_clip_list();
    draw_properties();
}

void AnimationEditorContext::setup_dockspace(ImGuiID main_area) {
    ImGuiID center = main_area;

    // Left panel: Editors + Bone Tree (top) + Clip List (bottom) — 14%
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.14f, nullptr, &center);
    ImGuiID left_bottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.50f, nullptr, &left);

    // Right panel: properties — 22%
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.22f, nullptr, &center);

    // Bottom: timeline + stats/console — 32%
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.32f, nullptr, &center);

    // Per-context shared panel names
    int ci = context_index();
    char stats[32], console[32], history[32], code[32];
    std::snprintf(stats, sizeof(stats), "Stats##ctx%d", ci);
    std::snprintf(console, sizeof(console), "Console##ctx%d", ci);
    std::snprintf(history, sizeof(history), "History##ctx%d", ci);
    std::snprintf(code, sizeof(code), "Code Editor##ctx%d", ci);

    ImGui::DockBuilderDockWindow("Bone Tree", left);
    ImGui::DockBuilderDockWindow("Clip List", left_bottom);
    ImGui::DockBuilderDockWindow(history, left_bottom);
    ImGui::DockBuilderDockWindow("Anim Preview", center);
    ImGui::DockBuilderDockWindow(code, center);
    ImGui::DockBuilderDockWindow("Anim Properties", right);
    ImGui::DockBuilderDockWindow("Anim Timeline", bottom);
    ImGui::DockBuilderDockWindow(stats, bottom);
    ImGui::DockBuilderDockWindow(console, bottom);
}

bool AnimationEditorContext::handle_shortcuts() {
    if (ImGui::GetIO().WantTextInput) return false;

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

    return false;
}

std::vector<ViewportRenderRequest> AnimationEditorContext::build_render_requests() {
    return viewport_manager_.build_render_requests();
}

void AnimationEditorContext::apply_resize(VulkanContext& vk) {
    viewport_manager_.apply_resize(vk);
}

bool AnimationEditorContext::any_viewport_hovered() const {
    return viewport_manager_.any_hovered();
}

// ── Open from asset ──────────────────────────────────────────────────────

void AnimationEditorContext::open_skeleton(const std::string& skel_path) {
    skeleton_path_ = skel_path;
    auto resolved = base_path::resolve_asset(skel_path, deps_.project_mgr->active());

    try {
        skeleton_ = std::make_shared<Skeleton>(Skeleton::load(resolved));
    } catch (const std::exception& e) {
        FJELL_CORE_ERROR("AnimEditor: failed to load skeleton: {}", e.what());
        return;
    }

    name_ = fs::path(skel_path).stem().string();
    selected_bone_ = -1;
    selected_clip_ = -1;
    current_clip_.reset();

    // Scan for .fjanim files in the same directory
    scan_clips();

    // Set up preview scene with a skeletal mesh
    preview_scene_.clear();

    // Look for a .fjmesh in the same directory
    auto skel_dir = fs::path(resolved).parent_path();
    auto stem = fs::path(resolved).stem().string();
    auto mesh_path = skel_dir / (stem + ".fjmesh");

    if (fs::exists(mesh_path)) {
        auto mesh_result = deps_.resource_manager->load_skeletal_model(mesh_path.string());
        if (mesh_result) {
            auto* skel_model = deps_.resource_manager->get(*mesh_result);
            if (skel_model) {
                auto node = std::make_unique<SceneNode>();
                node->name = stem;

                auto& smc = node->components.add<SkeletalMeshComponent>();
                smc.skeletal_model_handle = *mesh_result;
                smc.skeletal_model = skel_model;
                smc.skeleton = skeleton_;
                smc.skeleton_path = skel_path;

                // Default material
                auto tex = deps_.resource_manager->load_texture(
                    base_path::resolve("engine_assets/textures/test.png")).value_or(TextureHandle{});
                smc.material_handle = deps_.resource_manager->create_material("anim_preview", tex);
                smc.material = deps_.resource_manager->get(smc.material_handle);

                smc.animator = std::make_unique<AnimatorComponent>();
                smc.animator->init(skeleton_.get());

                preview_node_id_ = preview_scene_.add_node(std::move(node));
            }
        }
    }

    // Auto-focus camera
    if (auto* cam = viewport_manager_.active_camera()) {
        cam->focus({0.0f, 1.0f, 0.0f}, 2.5f);
    }

    // Play first clip if available
    if (!available_clip_paths_.empty()) {
        load_clip(0);
    }

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

// ── Panels ───────────────────────────────────────────────────────────────

void AnimationEditorContext::draw_bone_tree() {
    if (ImGui::Begin("Bone Tree")) {
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

            // Recursive tree draw
            std::function<void(int)> draw_joint = [&](int idx) {
                ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                           ImGuiTreeNodeFlags_SpanAvailWidth;
                if (children[idx].empty()) {
                    flags |= ImGuiTreeNodeFlags_Leaf;
                }
                if (idx == selected_bone_) {
                    flags |= ImGuiTreeNodeFlags_Selected;
                }

                bool open = ImGui::TreeNodeEx(joints[idx].name.c_str(), flags);
                if (ImGui::IsItemClicked()) {
                    selected_bone_ = idx;
                }
                if (open) {
                    for (int child : children[idx]) {
                        draw_joint(child);
                    }
                    ImGui::TreePop();
                }
            };

            for (int root : roots) {
                draw_joint(root);
            }
        }
    }
    ImGui::End();
}

void AnimationEditorContext::draw_clip_list() {
    if (ImGui::Begin("Clip List")) {
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
    if (ImGui::Begin("Anim Properties")) {
        if (skeleton_ && selected_bone_ >= 0 &&
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

    try {
        current_clip_ = std::make_shared<AnimationClip>(AnimationClip::load(resolved));
    } catch (const std::exception& e) {
        FJELL_CORE_ERROR("AnimEditor: failed to load clip: {}", e.what());
        return;
    }

    // Update timeline
    timeline_.set_duration(current_clip_->duration());
    timeline_.set_time(0.0f);
    timeline_.set_playing(true);

    // Play on the preview animator
    auto* node = preview_scene_.find(preview_node_id_);
    if (!node) return;
    auto* smc = node->components.get<SkeletalMeshComponent>();
    if (!smc || !smc->animator) return;

    smc->animator->play(current_clip_.get());
}

void AnimationEditorContext::update_preview(float dt) {
    if (preview_node_id_ == 0) return;

    auto* node = preview_scene_.find(preview_node_id_);
    if (!node) return;
    auto* smc = node->components.get<SkeletalMeshComponent>();
    if (!smc || !smc->animator) return;

    // Sync animator time with timeline
    if (timeline_.playing()) {
        smc->animator->set_time(timeline_.time());
    }

    // Tick animation + world matrices on the preview scene
    preview_scene_.compute_world_matrices(nullptr);
    auto& system = deps_.animation_subsystem->system();
    system.update(preview_scene_, dt);
}

} // namespace fjell
