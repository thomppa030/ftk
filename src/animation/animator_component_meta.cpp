#include "scene/component_registry.hpp"
#include "animation/animator_component.hpp"
#include "scene/node.hpp"
#include "ui/asset_ref_widget.hpp"
#include "renderer/components/skeletal_mesh_component.hpp"
#include "renderer/resources/skeletal_model.hpp"
#include "animation/animation_clip.hpp"
#include "animation/skeleton.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"
#include "core/project_manager.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <cstdio>

namespace fjell {

static const bool registered = [] {
    ComponentRegistry::instance().register_component<AnimatorComponent>({
        .display_name = "Animator",
        .json_key = "animator",
        .color = {0.8f, 0.5f, 0.2f, 1.0f},

        .draw = [](Node3D& node, void* ptr, const InspectorContext& ctx) -> bool {
            auto* anim = static_cast<AnimatorComponent*>(ptr);
            bool changed = false;
            if (auto* skel = node.components.get<SkeletalMeshComponent>()) {
                if (skel->skeletal_model && skel->skeletal_model->skeleton()) {
                    ImGui::TextDisabled("Skeleton: %d joints (from Skeletal Mesh)", skel->skeletal_model->skeleton()->num_joints());
                    if (!anim->initialized()) anim->init(skel->skeletal_model->skeleton());
                } else {
                    ImGui::TextDisabled("Skeleton: (none — assign a Skeletal Mesh first)");
                }
            } else {
                ImGui::TextDisabled("Skeleton: (no Skeletal Mesh component)");
            }
            static const std::string animset_exts[] = {".fjanimset"};
            static const std::string clip_exts[] = {".fjanim"};
            if (asset_ref_field({.label = "Animation Set", .id = "##AnimAnimset", .extensions = animset_exts, .ctx = &ctx}, anim->animset_path)) {
                if (!anim->animset_path.empty() && ctx.project_mgr) {
                    auto resolved = base_path::resolve_asset(anim->animset_path, ctx.project_mgr->active());
                    if (auto r = anim->load_state_machine(resolved); !r) {
                        FJELL_CORE_ERROR("Failed to load animset: {}", r.error());
                    }
                    for (size_t i = 0; i < anim->clips.size(); ++i) {
                        if (i < anim->clip_paths.size()) anim->set_clip(anim->clip_paths[i], anim->clips[i].get());
                    }
                    std::string base_dir = ctx.project_mgr->active() ? ctx.project_mgr->active()->root().string() : "";
                    anim->resolve_blend_spaces(base_dir);
                    changed = true;
                }
            }
            ImGui::Text("Clips");
            int remove_idx = -1;
            for (size_t i = 0; i < anim->clip_paths.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                char label[32];
                std::snprintf(label, sizeof(label), "Clip %zu", i);
                if (asset_ref_field({.label = label, .id = "##ClipPath", .extensions = clip_exts, .ctx = &ctx}, anim->clip_paths[i])) {
                    if (ctx.project_mgr && !anim->clip_paths[i].empty()) {
                        auto resolved = base_path::resolve_asset(anim->clip_paths[i], ctx.project_mgr->active());
                        auto result = AnimationClip::load(resolved);
                        if (result) {
                            auto clip = std::make_shared<AnimationClip>(std::move(*result));
                            if (i < anim->clips.size()) anim->clips[i] = clip;
                            else anim->clips.push_back(clip);
                            anim->set_clip(anim->clip_paths[i], clip.get());
                            if (!anim->has_active_clip()) anim->play(clip.get());
                            changed = true;
                        } else {
                            FJELL_CORE_ERROR("Failed to load clip: {}", result.error());
                        }
                    }
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("X")) remove_idx = static_cast<int>(i);
                ImGui::PopID();
            }
            if (remove_idx >= 0) {
                anim->clip_paths.erase(anim->clip_paths.begin() + remove_idx);
                if (static_cast<size_t>(remove_idx) < anim->clips.size()) anim->clips.erase(anim->clips.begin() + remove_idx);
                changed = true;
            }
            if (ImGui::Button("+ Add Clip", {-1, 0})) anim->clip_paths.emplace_back();
            ImGui::Spacing();
            ImGui::TextDisabled("Initialized: %s", anim->initialized() ? "yes" : "no");
            ImGui::TextDisabled("Bones: %u", anim->bone_count());
            return changed;
        },

        .init = [](Node3D& node, const InspectorContext& /*ctx*/) {
            node.components.add<AnimatorComponent>();
        },

        .serialize = [](const void* ptr, const Node& /*node*/, nlohmann::json& out) {
            auto* anim = static_cast<const AnimatorComponent*>(ptr);
            if (!anim->clip_paths.empty()) out["clips"] = anim->clip_paths;
            if (!anim->animset_path.empty()) out["animset"] = anim->animset_path;
        },

        .deserialize = [](Node& node, const nlohmann::json& j, ResourceManager& /*res*/, const Project* proj) {
            if (node.components.has<AnimatorComponent>()) return;
            auto& anim = node.components.add<AnimatorComponent>();
            if (const auto* smc = node.components.get<SkeletalMeshComponent>()) {
                if (smc->skeletal_model && smc->skeletal_model->skeleton()) anim.init(smc->skeletal_model->skeleton());
            }
            if (j.contains("clips") && j["clips"].is_array()) {
                for (const auto& jc : j["clips"]) {
                    std::string clip_rel = jc.get<std::string>();
                    anim.clip_paths.push_back(clip_rel);
                    auto result = AnimationClip::load(base_path::resolve_asset(clip_rel, proj));
                    if (result) {
                        auto clip = std::make_shared<AnimationClip>(std::move(*result));
                        anim.clips.push_back(clip);
                        if (anim.clips.size() == 1) anim.play(clip.get());
                    } else {
                        FJELL_CORE_ERROR("Failed to load clip {}: {}", clip_rel, result.error());
                    }
                }
            }
            if (j.contains("animset")) {
                anim.animset_path = j["animset"].get<std::string>();
                auto resolved = base_path::resolve_asset(anim.animset_path, proj);
                if (auto r = anim.load_state_machine(resolved); !r) {
                    FJELL_CORE_ERROR("Failed to load animset {}: {}", anim.animset_path, r.error());
                }
                for (size_t i = 0; i < anim.clips.size(); ++i) {
                    if (i < anim.clip_paths.size()) anim.set_clip(anim.clip_paths[i], anim.clips[i].get());
                }
                std::string base_dir = proj ? proj->root().string() : "";
                anim.resolve_blend_spaces(base_dir);
            }
        },

        .script_type_name = {},
    });
    return true;
}();

} // namespace fjell
