#include "scene/component_registry.hpp"
#include "renderer/arrow_component.hpp"
#include "scene/node.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

namespace fjell {

static const bool registered = [] {
    ComponentRegistry::instance().register_component<ArrowComponent>({
        .display_name = "Arrow",
        .json_key = "arrow",
        .color = {0.9f, 0.75f, 0.3f, 1.0f},

        .draw = [](Node3D& /*node*/, void* ptr, const InspectorContext& /*ctx*/) -> bool {
            auto* arr = static_cast<ArrowComponent*>(ptr);
            bool changed = false;
            ImGui::Text("Length");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat("##ArrowLen", &arr->length, 0.1f, 0.1f, 50.0f))
                changed = true;
            ImGui::Text("Color");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::ColorEdit3("##ArrowColor", &arr->color.x))
                changed = true;
            return changed;
        },

        .init = [](Node3D& node, const InspectorContext& /*ctx*/) {
            node.components.add<ArrowComponent>();
        },

        .serialize = [](const void* ptr, const Node& /*node*/, nlohmann::json& out) {
            auto* arr = static_cast<const ArrowComponent*>(ptr);
            out["length"] = arr->length;
            out["color"] = {arr->color.x, arr->color.y, arr->color.z};
        },

        .deserialize = [](Node& node, const nlohmann::json& j, ResourceManager& /*res*/, const Project* /*proj*/) {
            auto& arr = node.components.add<ArrowComponent>();
            arr.length = j.value("length", 2.0f);
            if (j.contains("color")) {
                auto& c = j["color"];
                arr.color = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>()};
            }
        },
    });
    return true;
}();

} // namespace fjell
