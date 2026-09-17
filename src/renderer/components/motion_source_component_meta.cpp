#include "renderer/components/motion_source_component.hpp"
#include "scene/component_registry.hpp"
#include "scene/node.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

namespace fjell {

static const bool registered = [] {
    ComponentRegistry::instance().register_component<MotionSourceComponent>({
        .display_name = "Motion Source",
        .json_key = "motion_source",
        .color = {0.45f, 0.70f, 0.80f, 1.0f},

        .draw = [](Node3D& /*node*/, void* ptr, const InspectorContext& /*ctx*/) -> bool {
            auto* ms = static_cast<MotionSourceComponent*>(ptr);
            bool changed = false;
            ImGui::Text("Radius"); ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat("##MotionRadius", &ms->radius, 0.05f, 0.0f, 20.0f, "%.2f m")) {
                changed = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("How wide a mark this leaves. 0 follows the node's own size.");
            }
            ImGui::Text("Height"); ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat("##MotionHeight", &ms->height, 0.05f, 0.0f, 20.0f, "%.2f m")) {
                changed = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("How high the movement reaches, so fog above a walker's head "
                                  "is left alone. 0 follows the node's own size.");
            }
            ImGui::Text("Strength"); ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat("##MotionStrength", &ms->strength, 0.01f, 0.0f, 4.0f)) {
                changed = true;
            }
            return changed;
        },

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
