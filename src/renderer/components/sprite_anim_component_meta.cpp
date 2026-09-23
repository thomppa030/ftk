#include "scene/component_registry.hpp"
#include "core/base_path.hpp"
#include "renderer/components/sprite_anim_component.hpp"
#include "scene/node.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace fjell {

static const bool registered = [] {
    ComponentRegistry::instance().register_component<SpriteAnimComponent>({
        .display_name = "Sprite Animation",
        .json_key = "sprite_anim",
        .color = {0.40f, 0.75f, 0.60f, 1.0f},

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
