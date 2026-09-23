#include "scene/component_registry.hpp"
#include "physics/physics_types.hpp"
#include "scene/node.hpp"

#include <nlohmann/json.hpp>

namespace fjell {

static const bool registered = [] {
    ComponentRegistry::instance().register_component<CharacterComponent>({
        .display_name = "Character Controller",
        .json_key = "character",
        .color = {0.2f, 0.8f, 0.8f, 1.0f},

        .init = [](Node3D& node, const InspectorContext& /*ctx*/) {
            node.components.add<CharacterComponent>();
        },

        .serialize = [](const void* ptr, const Node& /*node*/, nlohmann::json& out,
                        const Project* /*proj*/) {
            auto* cc = static_cast<const CharacterComponent*>(ptr);
            out["capsule_radius"] = cc->capsule_radius;
            out["capsule_height"] = cc->capsule_height;
            out["max_slope_angle"] = cc->max_slope_angle;
            out["mass"] = cc->mass;
            out["feet_at_origin"] = cc->feet_at_origin;
            out["wind_push"] = cc->wind_push;
            out["shape_offset"] = {cc->shape_offset.x, cc->shape_offset.y, cc->shape_offset.z};
            out["step_up"] = cc->step_up;
            out["step_down"] = cc->step_down;
            out["channel"] = collision_channel_name(cc->filter.channel);
            out["block_mask"] = cc->filter.block_mask;
            out["overlap_mask"] = cc->filter.overlap_mask;
        },

        .deserialize = [](Node& node, const nlohmann::json& j, ResourceManager& /*res*/, const Project* /*proj*/) {
            auto& cc = node.components.add<CharacterComponent>();
            cc.capsule_radius = j.value("capsule_radius", 0.3f);
            cc.capsule_height = j.value("capsule_height", 1.4f);
            cc.max_slope_angle = j.value("max_slope_angle", 50.0f);
            cc.mass = j.value("mass", 80.0f);
            cc.feet_at_origin = j.value("feet_at_origin", true);
            cc.wind_push = j.value("wind_push", 0.0f);
            if (j.contains("shape_offset")) {
                auto& o = j["shape_offset"];
                cc.shape_offset = {o[0].get<float>(), o[1].get<float>(), o[2].get<float>()};
            }
            cc.step_up = j.value("step_up", 0.4f);
            cc.step_down = j.value("step_down", 0.5f);
            // Stored by name, like a collider's. A character saved before it
            // had a filter keeps the pawn defaults.
            cc.filter.channel = CollisionChannel::pawn;
            collision_channel_from_name(j.value("channel", std::string{}), cc.filter.channel);
            auto defaults = default_filter_for(cc.filter.channel);
            cc.filter.block_mask = j.value("block_mask", defaults.block_mask);
            cc.filter.overlap_mask = j.value("overlap_mask", defaults.overlap_mask);
        },

        .script_type_name = "CharacterComponent",
        .script_get = [](Node& node) -> void* { return node.components.get<CharacterComponent>(); },
        .script_add = [](Node& node) -> void* { return &node.components.add<CharacterComponent>(); },
    });
    return true;
}();

} // namespace fjell
