#include "scene/component_registry.hpp"
#include "physics/buoyancy.hpp"
#include "physics/physics_types.hpp"
#include "scene/node.hpp"

#include <nlohmann/json.hpp>


namespace fjell {

static const bool registered = [] {
    ComponentRegistry::instance().register_component<BuoyancyComponent>({
        .display_name = "Buoyancy",
        .json_key = "buoyancy",

        .init = [](Node3D& node, const InspectorContext& /*ctx*/) {
            node.components.add<BuoyancyComponent>();
        },

        .serialize = [](const void* ptr, const Node& /*node*/, nlohmann::json& out,
                        const Project* /*proj*/) {
            auto* b = static_cast<const BuoyancyComponent*>(ptr);
            out["density"] = b->density;
            out["drag"] = b->drag;
            out["angular_drag"] = b->angular_drag;
            out["follow_current"] = b->follow_current;
            // Only written when authored, so a body floating on its collider
            // does not carry an empty list in every scene file.
            if (!b->points.empty()) {
                auto& points = out["points"] = nlohmann::json::array();
                for (const auto& p : b->points) points.push_back({p.x, p.y, p.z});
            }
        },

        .deserialize = [](Node& node, const nlohmann::json& j, ResourceManager& /*res*/,
                          const Project* /*proj*/) {
            auto& b = node.components.add<BuoyancyComponent>();
            b.density = j.value("density", 0.6f);
            b.drag = j.value("drag", 1.2f);
            b.angular_drag = j.value("angular_drag", 0.8f);
            b.follow_current = j.value("follow_current", true);
            if (j.contains("points") && j["points"].is_array()) {
                for (const auto& p : j["points"]) {
                    if (p.is_array() && p.size() == 3) {
                        b.points.emplace_back(p[0].get<float>(), p[1].get<float>(),
                                              p[2].get<float>());
                    }
                }
            }
        },

        .script_type_name = "BuoyancyComponent",
        .script_get = [](Node& node) -> void* { return node.components.get<BuoyancyComponent>(); },
        .script_add = [](Node& node) -> void* { return &node.components.add<BuoyancyComponent>(); },
    });
    return true;
}();

} // namespace fjell
