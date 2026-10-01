#include "scene/component_registry.hpp"
#include "renderer/components/skeletal_mesh_component.hpp"
#include "scene/node.hpp"
#include "core/base_path.hpp"
#include "ftk/base/log.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/resources/resource_handle.hpp"
#include "core/project_manager.hpp"
#include "renderer/resources/material.hpp"

#include <nlohmann/json.hpp>

namespace fjell {

static const bool registered = [] {
    ComponentRegistry::instance().register_component<SkeletalMeshComponent>({
        .display_name = "Skeletal Mesh",
        .json_key = "skeletal_mesh",

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
