#include "scene/component_registry.hpp"
#include "physics/physics_types.hpp"
#include "renderer/components/mesh_component.hpp"
#include "scene/node.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

namespace fjell {

static const bool registered = [] {
    ComponentRegistry::instance().register_component<ColliderComponent>({
        .display_name = "Collider",
        .json_key = "collider",
        .color = {0.85f, 0.45f, 0.25f, 1.0f},

        .draw = [](Node3D& node, void* ptr, const InspectorContext& /*ctx*/) -> bool {
            auto* col = static_cast<ColliderComponent*>(ptr);
            bool changed = false;

            bool has_mesh = node.components.has<MeshComponent>();
            ImGui::BeginDisabled(!has_mesh);
            if (ImGui::Checkbox("Inherit From Mesh", &col->inherit_from_mesh)) changed = true;
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip(has_mesh
                    ? "Use the collision authored on the mesh asset"
                    : "Needs a Mesh component");
            }
            if (col->inherit_from_mesh && has_mesh) {
                ImGui::TextDisabled("Collision comes from the mesh asset.");
                if (ImGui::Checkbox("Is Trigger", &col->is_trigger)) changed = true;
                return changed;
            }

            static const char* shape_labels[] = {"Box", "Sphere", "Capsule", "Mesh"};
            int sh = static_cast<int>(col->shape);
            ImGui::Text("Shape"); ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##Shape", &sh, shape_labels, 4)) { col->shape = static_cast<ColliderShape>(sh); changed = true; }
            switch (col->shape) {
                case ColliderShape::Box:
                    ImGui::Text("Half Extents"); ImGui::SetNextItemWidth(-1);
                    if (ImGui::DragFloat3("##HalfExtents", &col->half_extents.x, 0.05f, 0.01f, 100.0f)) changed = true;
                    break;
                case ColliderShape::Sphere:
                    ImGui::Text("Radius"); ImGui::SetNextItemWidth(-1);
                    if (ImGui::DragFloat("##Radius", &col->radius, 0.05f, 0.01f, 100.0f)) changed = true;
                    break;
                case ColliderShape::Capsule:
                    ImGui::Text("Radius"); ImGui::SetNextItemWidth(-1);
                    if (ImGui::DragFloat("##RadiusCap", &col->radius, 0.05f, 0.01f, 100.0f)) changed = true;
                    ImGui::Text("Height"); ImGui::SetNextItemWidth(-1);
                    if (ImGui::DragFloat("##Height", &col->height, 0.05f, 0.01f, 100.0f)) changed = true;
                    break;
                case ColliderShape::Mesh:
                    ImGui::TextDisabled("Uses model collision mesh");
                    break;
            }
            ImGui::Text("Offset"); ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat3("##Offset", &col->offset.x, 0.05f)) changed = true;
            if (ImGui::Checkbox("Is Trigger", &col->is_trigger)) changed = true;
            return changed;
        },

        .init = [](Node3D& node, const InspectorContext& /*ctx*/) {
            auto& col = node.components.add<ColliderComponent>();
            if (node.source == "primitive:cube") col.shape = ColliderShape::Box;
            else if (node.source == "primitive:sphere") { col.shape = ColliderShape::Sphere; col.radius = 0.5f; }
            else col.shape = ColliderShape::Box;
        },

        .serialize = [](const void* ptr, const Node& /*node*/, nlohmann::json& out) {
            auto* col = static_cast<const ColliderComponent*>(ptr);
            out["shape"] = static_cast<int>(col->shape);
            out["inherit_from_mesh"] = col->inherit_from_mesh;
            out["half_extents"] = {col->half_extents.x, col->half_extents.y, col->half_extents.z};
            out["radius"] = col->radius;
            out["height"] = col->height;
            out["offset"] = {col->offset.x, col->offset.y, col->offset.z};
            out["is_trigger"] = col->is_trigger;
        },

        .deserialize = [](Node& node, const nlohmann::json& j, ResourceManager& /*res*/, const Project* /*proj*/) {
            auto& col = node.components.add<ColliderComponent>();
            col.shape = static_cast<ColliderShape>(j.value("shape", 0));
            col.inherit_from_mesh = j.value("inherit_from_mesh", false);
            if (j.contains("half_extents")) { auto& he = j["half_extents"]; col.half_extents = {he[0].get<float>(), he[1].get<float>(), he[2].get<float>()}; }
            col.radius = j.value("radius", 0.5f);
            col.height = j.value("height", 1.0f);
            if (j.contains("offset")) { auto& o = j["offset"]; col.offset = {o[0].get<float>(), o[1].get<float>(), o[2].get<float>()}; }
            col.is_trigger = j.value("is_trigger", false);
        },

        .script_type_name = "ColliderComponent",
        .script_get = [](Node& node) -> void* { return node.components.get<ColliderComponent>(); },
        .script_add = [](Node& node) -> void* { return &node.components.add<ColliderComponent>(); },
    });
    return true;
}();

} // namespace fjell
