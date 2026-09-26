#include "scene/component_registry.hpp"
#include "renderer/components/mesh_component.hpp"
#include "scene/node.hpp"
#include "core/base_path.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "core/project_manager.hpp"
#include "renderer/resources/material.hpp"

#include <nlohmann/json.hpp>

namespace fjell {

static const bool registered = [] {
    ComponentRegistry::instance().register_component<MeshComponent>({
        .display_name = "Mesh",
        .json_key = "mesh",

        .init = [](Node3D& node, const InspectorContext& /*ctx*/) {
            node.components.add<MeshComponent>();
        },

        .serialize = [](const void* ptr, const Node& /*node*/, nlohmann::json& out,
                        const Project* proj) {
            auto* mesh = static_cast<const MeshComponent*>(ptr);
            out["material"] = base_path::asset_reference(mesh->material_name, proj);
            if (mesh->material) {
                const auto& bcf = mesh->material->params().base_color_factor;
                if (bcf.r != 1.0f || bcf.g != 1.0f || bcf.b != 1.0f || bcf.a != 1.0f) {
                    if (bcf.a < 1.0f) out["color"] = {bcf.r, bcf.g, bcf.b, bcf.a};
                    else out["color"] = {bcf.r, bcf.g, bcf.b};
                }
            }
        },

        .deserialize = [](Node& node, const nlohmann::json& j, ResourceManager& /*res*/, const Project* /*proj*/) {
            // MeshComponent deserialization happens at node creation level in node_serializer.cpp
            // This only handles the case where the component already exists
            if (!node.components.has<MeshComponent>()) {
                node.components.add<MeshComponent>();
            }
            auto* mesh = node.components.get<MeshComponent>();
            if (mesh) {
                mesh->material_name = j.value("material", "");
            }
        },

        .material = [](void* ptr) -> Material* {
            return static_cast<MeshComponent*>(ptr)->material;
        },
        .script_type_name = "MeshComponent",
        .script_add = [](Node& node) -> void* { return &node.components.add<MeshComponent>(); },
    });
    return true;
}();

} // namespace fjell
