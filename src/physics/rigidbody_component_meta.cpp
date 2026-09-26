#include "scene/component_registry.hpp"
#include "physics/physics_types.hpp"
#include "scene/node.hpp"

#include <nlohmann/json.hpp>

namespace fjell {

static const bool registered = [] {
    ComponentRegistry::instance().register_component<RigidbodyComponent>({
        .display_name = "Rigidbody",
        .json_key = "rigidbody",

        .init = [](Node3D& node, const InspectorContext& /*ctx*/) {
            node.components.add<RigidbodyComponent>();
        },

        .serialize = [](const void* ptr, const Node& /*node*/, nlohmann::json& out,
                        const Project* /*proj*/) {
            auto* rb = static_cast<const RigidbodyComponent*>(ptr);
            out["body_type"] = static_cast<int>(rb->body_type);
            out["mass"] = rb->mass;
            out["friction"] = rb->friction;
            out["restitution"] = rb->restitution;
            out["linear_damping"] = rb->linear_damping;
            out["angular_damping"] = rb->angular_damping;
            out["gravity_enabled"] = rb->gravity_enabled;
            out["wind_area"] = rb->wind_area;
        },

        .deserialize = [](Node& node, const nlohmann::json& j, ResourceManager& /*res*/, const Project* /*proj*/) {
            auto& rb = node.components.add<RigidbodyComponent>();
            rb.body_type = static_cast<BodyType>(j.value("body_type", 2));
            rb.mass = j.value("mass", 1.0f);
            rb.friction = j.value("friction", 0.5f);
            rb.restitution = j.value("restitution", 0.3f);
            rb.linear_damping = j.value("linear_damping", 0.05f);
            rb.angular_damping = j.value("angular_damping", 0.05f);
            rb.gravity_enabled = j.value("gravity_enabled", true);
            rb.wind_area = j.value("wind_area", 0.0f);
        },

        .script_type_name = "RigidbodyComponent",
        .script_add = [](Node& node) -> void* { return &node.components.add<RigidbodyComponent>(); },
    });
    return true;
}();

} // namespace fjell
