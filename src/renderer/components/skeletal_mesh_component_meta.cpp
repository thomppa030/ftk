#include "scene/component_registry.hpp"
#include "renderer/components/skeletal_mesh_component.hpp"
#include "scene/node.hpp"
#include "ui/asset_ref_widget.hpp"
#include "renderer/resources/asset_shader_type.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/resources/resource_handle.hpp"
#include "core/project_manager.hpp"
#include "renderer/resources/material.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

namespace fjell {

static const bool registered = [] {
    ComponentRegistry::instance().register_component<SkeletalMeshComponent>({
        .display_name = "Skeletal Mesh",
        .json_key = "skeletal_mesh",
        .color = {0.5f, 0.3f, 0.8f, 1.0f},

        .draw = [](Node3D& node, void* ptr, const InspectorContext& ctx) -> bool {
            auto* skel = static_cast<SkeletalMeshComponent*>(ptr);
            bool changed = false;
            static const std::string model_exts[] = {".fjmesh", ".fjskel"};
            static const std::string material_exts[] = {".fjmat"};
            std::string skel_model_path = node.source;
            if (asset_ref_field({.label = "Model", .id = "##SkelModel", .extensions = model_exts, .ctx = &ctx}, skel_model_path)) {
                if (!skel_model_path.empty() && ctx.resource_manager && ctx.project_mgr) {
                    auto resolved = base_path::resolve_asset(skel_model_path, ctx.project_mgr->active());
                    auto result = ctx.resource_manager->load_skeletal_model(resolved);
                    if (result) {
                        skel->skeletal_model_handle = *result;
                        skel->skeletal_model = ctx.resource_manager->get(skel->skeletal_model_handle);
                    }
                }
                node.source = skel_model_path;
                changed = true;
            }
            if (asset_ref_field({.label = "Material", .id = "##SkelMaterial", .extensions = material_exts, .ctx = &ctx, .shader_types = only_shader_type(ShaderType::Spatial)}, skel->material_name)) {
                if (skel->material_name.empty()) {
                    skel->material_name = "default";
                }
                if (skel->material_name != "default" && ctx.resource_manager && ctx.project_mgr) {
                    auto resolved = base_path::resolve_asset(skel->material_name, ctx.project_mgr->active());
                    auto result = ctx.resource_manager->load_material_asset(resolved, ctx.project_mgr->active());
                    if (result) {
                        skel->material_handle = *result;
                        skel->material = ctx.resource_manager->get(skel->material_handle);
                    }
                } else if (skel->material_name == "default" && ctx.resource_manager) {
                    auto tex = ctx.resource_manager->default_white_handle();
                    skel->material_handle = ctx.resource_manager->create_material("default", tex);
                    skel->material = ctx.resource_manager->get(skel->material_handle);
                }
                changed = true;
            }
            return changed;
        },

        .init = [](Node3D& node, const InspectorContext& ctx) {
            auto& smc = node.components.add<SkeletalMeshComponent>();
            if (ctx.resource_manager) {
                auto tex = ctx.resource_manager->load_texture(base_path::resolve("engine_assets/textures/test.png")).value_or(TextureHandle{});
                smc.material_handle = ctx.resource_manager->create_material("skel_default", tex);
                smc.material = ctx.resource_manager->get(smc.material_handle);
                smc.material_name = "default";
            }
        },

        .serialize = [](const void* ptr, const Node& /*node*/, nlohmann::json& out,
                        const Project* proj) {
            auto* skel = static_cast<const SkeletalMeshComponent*>(ptr);
            out["material"] = base_path::asset_reference(skel->material_name, proj);
        },

        .deserialize = [](Node& node, const nlohmann::json& j, ResourceManager& /*res*/, const Project* /*proj*/) {
            // SkeletalMeshComponent deserialization happens at node creation level
            // This only handles the case where the component already exists
            if (!node.components.has<SkeletalMeshComponent>()) {
                node.components.add<SkeletalMeshComponent>();
            }
            auto* skel = node.components.get<SkeletalMeshComponent>();
            if (skel) {
                skel->material_name = j.value("material", "");
            }
        },

        .material = [](void* ptr) -> Material* {
            return static_cast<SkeletalMeshComponent*>(ptr)->material;
        },
        .script_type_name = {},
    });
    return true;
}();

} // namespace fjell
