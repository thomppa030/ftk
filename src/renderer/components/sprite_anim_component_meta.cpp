#include "scene/component_registry.hpp"
#include "core/base_path.hpp"
#include "renderer/components/sprite_anim_component.hpp"
#include "scene/node.hpp"
#include "ui/asset_ref_widget.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <cstring>
#include <string>

namespace fjell {

static const bool registered = [] {
    ComponentRegistry::instance().register_component<SpriteAnimComponent>({
        .display_name = "Sprite Animation",
        .json_key = "sprite_anim",
        .color = {0.40f, 0.75f, 0.60f, 1.0f},

        .draw = [](Node3D& /*node*/, void* ptr, const InspectorContext& ctx) -> bool {
            auto* sa = static_cast<SpriteAnimComponent*>(ptr);
            bool changed = false;
            static const std::string animset_exts[] = {".fjanimset"};
            static const std::string clip_exts[] = {".fjanim"};
            if (asset_ref_field({.label = "Animset", .id = "##SpriteAnimset", .extensions = animset_exts, .ctx = &ctx}, sa->animset_path)) { sa->loaded = false; changed = true; }
            ImGui::Separator();
            ImGui::Text("Legacy Animations");
            for (auto& [name, path] : sa->animations) {
                std::string label = "##SpriteAnim_" + name;
                if (asset_ref_field({.label = name.c_str(), .id = label.c_str(), .extensions = clip_exts, .ctx = &ctx}, path)) { sa->loaded = false; changed = true; }
            }
            static char new_anim_name[64] = "";
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.4f);
            ImGui::InputText("##NewAnimName", new_anim_name, sizeof(new_anim_name));
            ImGui::SameLine();
            if (ImGui::Button("Add Animation") && new_anim_name[0] != '\0') {
                sa->animations[new_anim_name] = "";
                sa->loaded = false; changed = true;
                new_anim_name[0] = '\0';
            }
            ImGui::Text("Default"); ImGui::SameLine(); ImGui::SetNextItemWidth(-1);
            char def_buf[128];
            std::strncpy(def_buf, sa->default_animation.c_str(), sizeof(def_buf) - 1);
            def_buf[sizeof(def_buf) - 1] = '\0';
            if (ImGui::InputText("##SpriteAnimDefault", def_buf, sizeof(def_buf))) {
                if (ImGui::IsItemDeactivatedAfterEdit()) { sa->default_animation = def_buf; changed = true; }
            }
            ImGui::Text("Playback Speed"); ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat("##SpriteAnimSpeed", &sa->playback_speed, 0.01f, 0.0f, 10.0f)) changed = true;
            return changed;
        },

        .init = [](Node3D& node, const InspectorContext& /*ctx*/) {
            node.components.add<SpriteAnimComponent>();
        },

        .serialize = [](const void* ptr, const Node& /*node*/, nlohmann::json& out,
                        const Project* proj) {
            auto* sa = static_cast<const SpriteAnimComponent*>(ptr);
            nlohmann::json anims;
            for (auto& [name, path] : sa->animations) anims[name] = path;
            if (!anims.empty()) out["animations"] = anims;
            if (!sa->animset_path.empty()) out["animset"] = base_path::asset_reference(sa->animset_path, proj);
            if (!sa->default_animation.empty()) out["default"] = sa->default_animation;
            if (sa->playback_speed != 1.0f) out["playback_speed"] = sa->playback_speed;
        },

        .deserialize = [](Node& node, const nlohmann::json& j, ResourceManager& /*res*/, const Project* /*proj*/) {
            auto& sa = node.components.add<SpriteAnimComponent>();
            sa.animset_path = j.value("animset", "");
            if (j.contains("animations")) { for (auto& [name, path] : j["animations"].items()) sa.animations[name] = path.get<std::string>(); }
            sa.default_animation = j.value("default", "");
            sa.playback_speed = j.value("playback_speed", 1.0f);
        },

        .script_type_name = "SpriteAnimComponent",
        .script_get = [](Node& node) -> void* { return node.components.get<SpriteAnimComponent>(); },
        .script_add = [](Node& node) -> void* { return &node.components.add<SpriteAnimComponent>(); },
    });
    return true;
}();

} // namespace fjell
