#include "scene/component_registry.hpp"
#include "physics/physics_types.hpp"
#include "scene/node.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

namespace fjell {

static const bool registered = [] {
    ComponentRegistry::instance().register_component<RigidbodyComponent>({
        .display_name = "Rigidbody",
        .json_key = "rigidbody",
        .color = {0.85f, 0.45f, 0.25f, 1.0f},

        .draw = [](Node3D& /*node*/, void* ptr, const InspectorContext& /*ctx*/) -> bool {
            auto* rb = static_cast<RigidbodyComponent*>(ptr);
            bool changed = false;
            static const char* body_type_labels[] = {"Static", "Kinematic", "Dynamic"};
            int bt = static_cast<int>(rb->body_type);
            ImGui::Text("Body Type");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##BodyType", &bt, body_type_labels, 3)) {
                rb->body_type = static_cast<BodyType>(bt);
                changed = true;
            }
            ImGui::Text("Mass"); ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat("##Mass", &rb->mass, 0.1f, 0.01f, 1000.0f)) changed = true;
            ImGui::Text("Friction"); ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat("##Friction", &rb->friction, 0.01f, 0.0f, 2.0f)) changed = true;
            ImGui::Text("Restitution"); ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat("##Restitution", &rb->restitution, 0.01f, 0.0f, 2.0f)) changed = true;
            ImGui::Text("Linear Damping"); ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat("##LinearDamping", &rb->linear_damping, 0.01f, 0.0f, 10.0f)) changed = true;
            ImGui::Text("Angular Damping"); ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat("##AngularDamping", &rb->angular_damping, 0.01f, 0.0f, 10.0f)) changed = true;
            if (ImGui::Checkbox("Gravity", &rb->gravity_enabled)) changed = true;
            return changed;
        },

        .init = [](Node3D& node, const InspectorContext& /*ctx*/) {
            node.components.add<RigidbodyComponent>();
        },

        .serialize = [](const void* ptr, const Node& /*node*/, nlohmann::json& out) {
            auto* rb = static_cast<const RigidbodyComponent*>(ptr);
            out["body_type"] = static_cast<int>(rb->body_type);
            out["mass"] = rb->mass;
            out["friction"] = rb->friction;
            out["restitution"] = rb->restitution;
            out["linear_damping"] = rb->linear_damping;
            out["angular_damping"] = rb->angular_damping;
            out["gravity_enabled"] = rb->gravity_enabled;
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
        },
    });
    return true;
}();

} // namespace fjell
