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

    viewport_manager_.draw(*deps_.vulkan_context, *deps_.input, dt);
    timeline_.draw(dt);
    draw_bone_tree();
    draw_clip_list();
    draw_properties();
    draw_state_graph();
    draw_parameters();
}

std::vector<std::string> AnimationEditorContext::docked_window_names() const {
    int ci = context_index();
    char stats[32], console[32], history[32], code[32];
    std::snprintf(stats, sizeof(stats), "Stats##ctx%d", ci);
    std::snprintf(console, sizeof(console), "Console##ctx%d", ci);
    std::snprintf(history, sizeof(history), "History##ctx%d", ci);
    std::snprintf(code, sizeof(code), "Code Editor##ctx%d", ci);
    return {"Bone Tree", "Clip List", history, "Anim Parameters",
            "Anim Preview", "State Graph", code, "Anim Properties",
            "Anim Timeline", stats, console};
}

void AnimationEditorContext::setup_dockspace(ImGuiID main_area) {
    ImGuiID center = main_area;

    // Left panel: Bone Tree (top) + Clip List + Params (bottom) — 14%
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.14f, nullptr, &center);
    ImGuiID left_mid = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.65f, nullptr, &left);
    ImGuiID left_bottom = ImGui::DockBuilderSplitNode(left_mid, ImGuiDir_Down, 0.50f, nullptr, &left_mid);

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
    ImGui::DockBuilderDockWindow("Clip List", left_mid);
    ImGui::DockBuilderDockWindow(history, left_mid);
    ImGui::DockBuilderDockWindow("Anim Parameters", left_bottom);
    ImGui::DockBuilderDockWindow("Anim Preview", center);
    ImGui::DockBuilderDockWindow("State Graph", center);
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

// ── Save ─────────────────────────────────────────────────────────────────

bool AnimationEditorContext::save() {
    if (animset_path_.empty()) return false;
    try {
        auto resolved = base_path::resolve_asset(animset_path_, deps_.project_mgr->active());
        state_machine_.save(resolved);
        FJELL_CORE_INFO("AnimEditor: saved animset {}", animset_path_);
        return true;
    } catch (const std::exception& e) {
        FJELL_CORE_ERROR("AnimEditor: failed to save animset: {}", e.what());
        return false;
    }
}

// ── Open from asset ──────────────────────────────────────────────────────

void AnimationEditorContext::open_skeleton(const std::string& skel_path) {
    skeleton_path_ = skel_path;
    auto resolved = base_path::resolve_asset(skel_path, deps_.project_mgr->active());

    name_ = fs::path(skel_path).stem().string();
    selected_bone_ = -1;
    selected_clip_ = -1;
    current_clip_.reset();

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
    try {
        state_machine_.load(resolved);
    } catch (const std::exception& e) {
        FJELL_CORE_ERROR("AnimEditor: failed to load animset {}: {}", animset_path, e.what());
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

    FJELL_CORE_INFO("AnimEditor: opened animset {} ({} states, {} params)",
                    animset_path,
                    static_cast<int>(state_machine_.states().size()),
                    static_cast<int>(state_machine_.params_mutable().size()));
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
    auto* anim = node->components.get<AnimatorComponent>();
    if (!anim) return;

    anim->play(current_clip_.get());
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
        try {
            auto clip = std::make_shared<AnimationClip>(AnimationClip::load(clip_resolved));
            anim->set_clip(state.clip, clip.get());
            anim->clips.push_back(std::move(clip));
            anim->clip_paths.push_back(state.clip);
        } catch (const std::exception& e) {
            FJELL_CORE_WARN("AnimEditor: failed to load clip {}: {}", state.clip, e.what());
        }
    }

    // Set up state machine on the preview animator
    anim->load_state_machine(animset_resolved_path);
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
    if (ImGui::Begin("State Graph")) {
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
    if (ImGui::Begin("Anim Parameters")) {
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
    try {
        state_machine_.save(resolved);
        anim->load_state_machine(resolved);

        // Re-wire clips
        for (const auto& state : state_machine_.states()) {
            if (state.clip.empty()) continue;
            auto clip_resolved = base_path::resolve_asset(state.clip, deps_.project_mgr->active());
            try {
                auto clip = std::make_shared<AnimationClip>(AnimationClip::load(clip_resolved));
                anim->set_clip(state.clip, clip.get());
                anim->clips.push_back(std::move(clip));
            } catch (...) {}
        }
    } catch (const std::exception& e) {
        FJELL_CORE_ERROR("AnimEditor: failed to sync preview: {}", e.what());
    }
}

} // namespace fjell
