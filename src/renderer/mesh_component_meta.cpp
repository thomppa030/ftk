#include "scene/component_registry.hpp"
#include "renderer/mesh_component.hpp"
#include "scene/node.hpp"
#include "ui/inspector_widgets.hpp"
#include "core/base_path.hpp"
#include "renderer/resource_manager.hpp"
#include "core/project_manager.hpp"
#include "renderer/material.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

namespace fjell {

static const bool registered = [] {
    ComponentRegistry::instance().register_component<MeshComponent>({
        .display_name = "Mesh",
        .json_key = "mesh",
        .color = {0.36f, 0.55f, 0.94f, 1.0f},

        .draw = [](Node3D& node, void* ptr, const InspectorContext& ctx) -> bool {
            auto* mesh = static_cast<MeshComponent*>(ptr);
            bool changed = false;
            std::string model_path = node.source;
            if (asset_path_field("Model", "##MeshModel", model_path)) {
                if (ctx.resource_manager && ctx.project_mgr) {
                    auto resolved = base_path::resolve_asset(model_path, ctx.project_mgr->active());
                    auto result = ctx.resource_manager->load_model(resolved);
                    if (result) {
                        mesh->model_handle = *result;
                        mesh->model = ctx.resource_manager->get(mesh->model_handle);
                        node.source = model_path;
                        changed = true;
                    }
                }
            }
            if (asset_path_field("Material", "##MeshMaterial", mesh->material_name)) {
                if (ctx.resource_manager && ctx.project_mgr) {
                    auto resolved = base_path::resolve_asset(mesh->material_name, ctx.project_mgr->active());
                    auto result = ctx.resource_manager->load_material_asset(resolved, ctx.project_mgr->active());
                    if (result) {
                        mesh->material_handle = *result;
                        mesh->material = ctx.resource_manager->get(mesh->material_handle);
                    }
                    changed = true;
                }
            }
            return changed;
        },

        .init = [](Node3D& node, const InspectorContext& /*ctx*/) {
            node.components.add<MeshComponent>();
        },

        .serialize = [](const void* ptr, const Node& /*node*/, nlohmann::json& out) {
            auto* mesh = static_cast<const MeshComponent*>(ptr);
            out["material"] = mesh->material_name;
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
    });
    return true;
}();

} // namespace fjell
