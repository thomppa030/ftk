#include "scene/component_registry.hpp"
#include "renderer/components/skeletal_mesh_component.hpp"
#include "scene/node.hpp"
#include "ui/inspector_widgets.hpp"
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
            std::string skel_model_path = node.source;
            if (asset_path_field("Model", "##SkelModel", skel_model_path)) {
                if (ctx.resource_manager && ctx.project_mgr) {
                    auto resolved = base_path::resolve_asset(skel_model_path, ctx.project_mgr->active());
                    auto result = ctx.resource_manager->load_skeletal_model(resolved);
                    if (result) {
                        skel->skeletal_model_handle = *result;
                        skel->skeletal_model = ctx.resource_manager->get(skel->skeletal_model_handle);
                        node.source = skel_model_path;
                        changed = true;
                    }
                }
            }
            if (asset_path_field("Material", "##SkelMaterial", skel->material_name)) {
                if (ctx.resource_manager && ctx.project_mgr) {
                    auto resolved = base_path::resolve_asset(skel->material_name, ctx.project_mgr->active());
                    auto result = ctx.resource_manager->load_material_asset(resolved, ctx.project_mgr->active());
                    if (result) {
                        skel->material_handle = *result;
                        skel->material = ctx.resource_manager->get(skel->material_handle);
                    }
                    changed = true;
                }
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

        .serialize = [](const void* ptr, const Node& /*node*/, nlohmann::json& out) {
            auto* skel = static_cast<const SkeletalMeshComponent*>(ptr);
            out["material"] = skel->material_name;
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
    });
    return true;
}();

} // namespace fjell
