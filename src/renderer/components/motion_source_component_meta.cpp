#include "renderer/components/motion_source_component.hpp"
#include "scene/component_registry.hpp"
#include "scene/node.hpp"

#include <nlohmann/json.hpp>

namespace fjell {

static const bool registered = [] {
    ComponentRegistry::instance().register_component<MotionSourceComponent>({
        .display_name = "Motion Source",
        .json_key = "motion_source",

        .init = [](Node3D& node, const InspectorContext& /*ctx*/) {
            node.components.add<MotionSourceComponent>();
        },

        .serialize = [](const void* ptr, const Node& /*node*/, nlohmann::json& out,
                        const Project* /*proj*/) {
            auto* ms = static_cast<const MotionSourceComponent*>(ptr);
            out["radius"] = ms->radius;
            out["height"] = ms->height;
            out["strength"] = ms->strength;
        },

        .deserialize = [](Node& node, const nlohmann::json& j, ResourceManager& /*res*/,
                          const Project* /*proj*/) {
            auto& ms = node.components.add<MotionSourceComponent>();
            ms.radius = j.value("radius", 0.0f);
            ms.height = j.value("height", 0.0f);
            ms.strength = j.value("strength", 1.0f);
        },

        .script_type_name = "MotionSourceComponent",
        .script_get = [](Node& node) -> void* { return node.components.get<MotionSourceComponent>(); },
        .script_add = [](Node& node) -> void* { return &node.components.add<MotionSourceComponent>(); },
    });
    return true;
}();

} // namespace fjell
