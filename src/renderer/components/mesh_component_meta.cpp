#include "scene/component_registry.hpp"
#include "renderer/components/mesh_component.hpp"
#include "scene/node.hpp"
#include "ui/asset_ref_widget.hpp"
#include "core/base_path.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "core/project_manager.hpp"
#include "renderer/resources/material.hpp"

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
            static const std::string model_exts[] = {".fjmesh"};
            static const std::string material_exts[] = {".fjmat"};
            std::string model_path = node.source;
            if (asset_ref_field({.label = "Model", .id = "##MeshModel", .extensions = model_exts, .ctx = &ctx}, model_path)) {
                if (!model_path.empty() && ctx.resource_manager && ctx.project_mgr) {
                    auto resolved = base_path::resolve_asset(model_path, ctx.project_mgr->active());
                    auto result = ctx.resource_manager->load_model(resolved);
                    if (result) {
                        mesh->model_handle = *result;
                        mesh->model = ctx.resource_manager->get(mesh->model_handle);
                    }
                }
                node.source = model_path;
                changed = true;
            }
            const auto* mat_type = AssetTypeRegistry::instance().find_by_name("Material");
            if (asset_ref_field({.label = "Material", .id = "##MeshMaterial", .extensions = material_exts, .ctx = &ctx, .asset_type = mat_type}, mesh->material_name)) {
                if (mesh->material_name.empty()) {
                    mesh->material_name = "default";
                }
                if (mesh->material_name != "default" && ctx.resource_manager && ctx.project_mgr) {
                    auto resolved = base_path::resolve_asset(mesh->material_name, ctx.project_mgr->active());
                    auto result = ctx.resource_manager->load_material_asset(resolved, ctx.project_mgr->active());
                    if (result) {
                        mesh->material_handle = *result;
                        mesh->material = ctx.resource_manager->get(mesh->material_handle);
                    }
                } else if (mesh->material_name == "default" && ctx.resource_manager) {
                    auto tex = ctx.resource_manager->default_white_handle();
                    mesh->material_handle = ctx.resource_manager->create_material("default", tex);
                    mesh->material = ctx.resource_manager->get(mesh->material_handle);
                }
                changed = true;
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

        .script_type_name = "MeshComponent",
        .script_get = [](Node& node) -> void* { return node.components.get<MeshComponent>(); },
        .script_add = [](Node& node) -> void* { return &node.components.add<MeshComponent>(); },
    });
    return true;
}();

} // namespace fjell
