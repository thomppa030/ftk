#include "core/base_path.hpp"
#include "ftk/base/log.hpp"
#include "renderer/components/terrain_component.hpp"
#include "core/project_manager.hpp"
#include "renderer/resources/material.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "scene/component_registry.hpp"
#include "scene/node.hpp"

#include <nlohmann/json.hpp>

namespace fjell {

void load_terrain_material(TerrainComponent& terrain, ResourceManager* resources,
                           const Project* project) {
    terrain.material_handle = {};
    terrain.material = nullptr;
    if (terrain.material_path.empty() || resources == nullptr) return;
    auto resolved = base_path::resolve_asset(terrain.material_path, project);
    auto handle = resources->load_material_asset(resolved, project);
    if (!handle) {
        FTK_CORE_ERROR("terrain material: {}", handle.error());
        return;
    }
    terrain.material_handle = *handle;
    terrain.material = resources->get(*handle);
}

static const bool registered = [] {
    ComponentRegistry::instance().register_component<TerrainComponent>({
        .display_name = "Terrain",
        .json_key = "terrain",

        .init = [](Node3D& node, const InspectorContext& /*ctx*/) {
            node.components.add<TerrainComponent>();
        },

        .serialize = [](const void* ptr, const Node& /*node*/, nlohmann::json& out,
                        const Project* proj) {
            const auto* terrain = static_cast<const TerrainComponent*>(ptr);
            out["terrain_path"] = terrain->terrain_path;
            if (!terrain->material_path.empty()) {
                out["material"] = base_path::asset_reference(terrain->material_path, proj);
            }
        },

        .deserialize = [](Node& node, const nlohmann::json& j, ResourceManager& res,
                          const Project* proj) {
            auto& terrain = node.components.add<TerrainComponent>();
            terrain.terrain_path = j.value("terrain_path", std::string{});
            terrain.material_path = j.value("material", std::string{});
            load_terrain_material(terrain, &res, proj);
            if (terrain.terrain_path.empty()) return;

            auto handle = res.load_terrain_asset(
                base_path::resolve_asset(terrain.terrain_path, proj), proj);
            if (!handle) {
                FTK_CORE_ERROR("terrain: {}", handle.error());
                return;
            }
            terrain.terrain_handle = *handle;
            terrain.terrain = res.get(*handle);
        },

        .material = [](void* ptr) -> Material* {
            return static_cast<TerrainComponent*>(ptr)->material;
        },
        .script_type_name = "TerrainComponent",
        .script_add = [](Node& node) -> void* { return &node.components.add<TerrainComponent>(); },
    });
    return true;
}();

} // namespace fjell
