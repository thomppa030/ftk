#include "scene/component_registry.hpp"
#include "physics/physics_types.hpp"
#include "scene/node.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

namespace fjell {

static const bool registered = [] {
    ComponentRegistry::instance().register_component<CharacterComponent>({
        .display_name = "Character Controller",
        .json_key = "character",
        .color = {0.2f, 0.8f, 0.8f, 1.0f},

        .draw = [](Node3D& /*node*/, void* ptr, const InspectorContext& /*ctx*/) -> bool {
            auto* cc = static_cast<CharacterComponent*>(ptr);
            bool changed = false;
            ImGui::Text("Capsule Radius"); ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat("##CharRadius", &cc->capsule_radius, 0.01f, 0.01f, 10.0f)) changed = true;
            ImGui::Text("Capsule Height"); ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat("##CharHeight", &cc->capsule_height, 0.01f, 0.01f, 20.0f)) changed = true;
            ImGui::Text("Max Slope Angle"); ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat("##CharSlope", &cc->max_slope_angle, 0.5f, 0.0f, 89.0f)) changed = true;
            ImGui::Text("Mass"); ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat("##CharMass", &cc->mass, 0.5f, 1.0f, 500.0f)) changed = true;
            static const char* ground_labels[] = {"On Ground", "On Steep Ground", "Not Supported", "In Air"};
            ImGui::TextDisabled("Ground: %s", ground_labels[static_cast<int>(cc->ground_state)]);
            return changed;
        },

        .init = [](Node3D& node, const InspectorContext& /*ctx*/) {
            node.components.add<CharacterComponent>();
        },

        .serialize = [](const void* ptr, const Node& /*node*/, nlohmann::json& out) {
            auto* cc = static_cast<const CharacterComponent*>(ptr);
            out["capsule_radius"] = cc->capsule_radius;
            out["capsule_height"] = cc->capsule_height;
            out["max_slope_angle"] = cc->max_slope_angle;
            out["mass"] = cc->mass;
        },

        .deserialize = [](Node& node, const nlohmann::json& j, ResourceManager& /*res*/, const Project* /*proj*/) {
            auto& cc = node.components.add<CharacterComponent>();
            cc.capsule_radius = j.value("capsule_radius", 0.3f);
            cc.capsule_height = j.value("capsule_height", 1.4f);
            cc.max_slope_angle = j.value("max_slope_angle", 50.0f);
            cc.mass = j.value("mass", 80.0f);
        },

        .script_type_name = "CharacterComponent",
        .script_get = [](Node& node) -> void* { return node.components.get<CharacterComponent>(); },
        .script_add = [](Node& node) -> void* { return &node.components.add<CharacterComponent>(); },
    });
    return true;
}();

} // namespace fjell
