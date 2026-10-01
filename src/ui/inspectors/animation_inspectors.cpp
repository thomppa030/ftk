#include "ui/inspectors/animation_inspectors.hpp"

#include "animation/animation_clip.hpp"
#include "animation/animator_component.hpp"
#include "animation/skeleton.hpp"
#include "core/project.hpp"
#include "ftk/base/log.hpp"
#include "ftk/ui/kit/feedback.hpp"
#include "ftk/ui/kit/field.hpp"
#include "ftk/ui/kit/list_editor.hpp"
#include "ftk/ui/kit/row.hpp"
#include "ftk/ui/kit/section.hpp"
#include "ftk/ui/kit/text_field.hpp"
#include "renderer/components/skeletal_mesh_component.hpp"
#include "renderer/components/sprite_anim_component.hpp"
#include "renderer/resources/skeletal_model.hpp"
#include "scene/node.hpp"
#include "ui/asset_ref_widget.hpp"
#include "ui/inspectors/asset_rows.hpp"

#include <imgui.h>

#include <cstdio>
#include <memory>
#include <optional>
#include <string>

namespace fjell {

ui::Edit draw_animator(Node3D& node, void* component, const InspectorContext& ctx) {
    auto& anim = *static_cast<AnimatorComponent*>(component);
    static const std::string animset_exts[] = {".fjanimset"};
    static const std::string clip_exts[] = {".fjanim"};
    const Project* project = project_of(ctx);
    ui::Edit edit;

    // The skeleton comes from the node's skeletal mesh, and the animator
    // takes it as soon as there is one.
    const Skeleton* skeleton = nullptr;
    if (const auto* skel = node.components.get<SkeletalMeshComponent>();
        skel != nullptr && skel->skeletal_model != nullptr) {
        skeleton = skel->skeletal_model->skeleton();
    }
    if (skeleton != nullptr && !anim.initialized()) anim.init(skeleton);
    if (node.components.get<SkeletalMeshComponent>() == nullptr) {
        ui::status(ui::Severity::Warning, "Needs a Skeletal Mesh to animate");
    } else if (skeleton == nullptr) {
        ui::status(ui::Severity::Warning, "The Skeletal Mesh has no skeleton yet");
    }

    if (auto t = ui::PropertyTable("##animator")) {
        if (skeleton != nullptr) {
            ui::row("Skeleton", [&] {
                char text[64];
                std::snprintf(text, sizeof(text), "%d joints, from the Skeletal Mesh", skeleton->num_joints());
                ui::readout(text);
            });
        }
        ui::row("Animation set", "The state machine that picks what plays: states, transitions and blend spaces.",
                [&] {
                    if (asset_slot({.label = "Animation set", .id = "##animset", .extensions = animset_exts,
                                    .ctx = &ctx},
                                   anim.animset_path)) {
                        if (!anim.animset_path.empty()) {
                            if (auto r = anim.load_state_machine(base_path::resolve_asset(anim.animset_path, project));
                                !r) {
                                FJELL_CORE_ERROR("Failed to load animset: {}", r.error());
                            }
                            for (size_t i = 0; i < anim.clips.size() && i < anim.clip_paths.size(); ++i) {
                                anim.set_clip(anim.clip_paths[i], anim.clips[i].get());
                            }
                            anim.resolve_blend_spaces(project != nullptr ? project->root().string() : "");
                        }
                        edit = {true, true};
                    }
                });
    }

    // The clips the set's states play, by path; the first plays when
    // nothing else has.
    ui::subheading("Clips");
    std::optional<std::size_t> remove;
    {
        ui::ListEditor list("##clips", anim.clip_paths.size(), {.reorder = false});
        for (std::size_t i = 0; i < anim.clip_paths.size(); ++i) {
            if (!list.begin_item(i)) continue;
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (asset_slot({.label = "Clip", .id = "##clip", .extensions = clip_exts, .ctx = &ctx},
                           anim.clip_paths[i])) {
                if (!anim.clip_paths[i].empty()) {
                    auto result = AnimationClip::load(base_path::resolve_asset(anim.clip_paths[i], project));
                    if (result) {
                        auto clip = std::make_shared<AnimationClip>(std::move(*result));
                        if (i < anim.clips.size()) anim.clips[i] = clip;
                        else anim.clips.push_back(clip);
                        anim.set_clip(anim.clip_paths[i], clip.get());
                        if (!anim.has_active_clip()) anim.play(clip.get());
                    } else {
                        FJELL_CORE_ERROR("Failed to load clip: {}", result.error());
                    }
                }
                edit = {true, true};
            }
            if (list.end_item()) remove = i;
        }
        if (anim.clip_paths.empty()) list.empty("No clips. Add the ones the animation set's states play");
        if (list.add_button("Add clip")) {
            anim.clip_paths.emplace_back();
            edit = {true, true};
        }
    }
    if (remove) {
        anim.clip_paths.erase(anim.clip_paths.begin() + static_cast<std::ptrdiff_t>(*remove));
        if (*remove < anim.clips.size()) anim.clips.erase(anim.clips.begin() + static_cast<std::ptrdiff_t>(*remove));
        edit = {true, true};
    }
    return edit;
}

ui::Edit draw_sprite_animation(Node3D& /*node*/, void* component, const InspectorContext& ctx) {
    auto& sa = *static_cast<SpriteAnimComponent*>(component);
    static const std::string animset_exts[] = {".fjanimset"};
    static const std::string clip_exts[] = {".fjanim"};
    ui::Edit edit;
    if (auto t = ui::PropertyTable("##sprite_animation")) {
        ui::row("Animation set", [&] {
            if (asset_slot({.label = "Animation set", .id = "##animset", .extensions = animset_exts, .ctx = &ctx},
                           sa.animset_path)) {
                sa.loaded = false;
                edit = {true, true};
            }
        });
        ui::row("Default", "The animation that plays first.", [&] {
            const ui::Edit e = ui::text_field("##default", sa.default_animation);
            if (e.committed) sa.loaded = false;
            edit |= e;
        });
        ui::row("Speed", [&] {
            edit |= ui::drag("##speed", sa.playback_speed,
                             {.speed = 0.01f, .min = 0.0f, .max = 10.0f, .unit = ui::Unit::Times});
        });
    }

    // Animations named one by one, from before animation sets.
    ui::subheading("Named animations");
    if (auto t = ui::PropertyTable("##named")) {
        for (auto& [name, path] : sa.animations) {
            ImGui::PushID(name.c_str());
            ui::row(name.c_str(), [&] {
                if (asset_slot({.label = name.c_str(), .id = "##path", .extensions = clip_exts, .ctx = &ctx}, path)) {
                    sa.loaded = false;
                    edit = {true, true};
                }
            });
            ImGui::PopID();
        }
        ui::row("Add animation", "Type the new animation's name and press Enter.", [&] {
            std::string name;
            if (ui::text_field("##new", name).committed && !name.empty() && !sa.animations.contains(name)) {
                sa.animations[name] = "";
                sa.loaded = false;
                edit = {true, true};
            }
        });
    }
    return edit;
}

} // namespace fjell
