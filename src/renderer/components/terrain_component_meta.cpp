#include "core/base_path.hpp"
#include "core/log.hpp"
#include "renderer/components/terrain_component.hpp"
#include "core/project_manager.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/resources/terrain_asset.hpp"
#include "ui/asset_ref_widget.hpp"
#include "scene/component_registry.hpp"
#include "scene/node.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

namespace fjell {

static const bool registered = [] {
    ComponentRegistry::instance().register_component<TerrainComponent>({
        .display_name = "Terrain",
        .json_key = "terrain",
        .color = {0.35f, 0.65f, 0.35f, 1.0f},

        .draw = [](Node3D& /*node*/, void* ptr, const InspectorContext& ctx) -> bool {
            auto* terrain = static_cast<TerrainComponent*>(ptr);
            bool changed = false;

            static const std::string terrain_exts[] = {".fjterrain"};
            if (asset_ref_field({.label = "Terrain Asset", .id = "##TerrainAsset",
                                 .extensions = terrain_exts, .ctx = &ctx},
                                terrain->terrain_path)) {
                // Resolve immediately: the path alone renders nothing, and the
                // inspector is the one place that changes it outside load.
                terrain->terrain_handle = {};
                terrain->terrain = nullptr;
                if (!terrain->terrain_path.empty() && ctx.resource_manager != nullptr &&
                    ctx.project_mgr != nullptr) {
                    auto resolved = base_path::resolve_asset(terrain->terrain_path,
                                                             ctx.project_mgr->active());
                    auto handle = ctx.resource_manager->load_terrain_asset(resolved);
                    if (handle) {
                        terrain->terrain_handle = *handle;
                        terrain->terrain = ctx.resource_manager->get(*handle);
                    } else {
                        FJELL_CORE_ERROR("terrain: {}", handle.error());
                    }
                }
                changed = true;
            }

            if (terrain->terrain != nullptr) {
                const auto& data = terrain->terrain->data();
                ImGui::TextDisabled("%u x %u samples, %u patches of %u quads",
                                    data.sample_count(), data.sample_count(),
                                    terrain->terrain->patches_per_side(),
                                    data.patch_quads());
                ImGui::TextDisabled("%.0f m across, height %.1f m",
                                    terrain->terrain->world_size(),
                                    terrain->terrain->height_scale());
            }

            if (ImGui::Checkbox("Show Wireframe", &terrain->show_wireframe)) changed = true;
            return changed;
        },

        .init = [](Node3D& node, const InspectorContext& /*ctx*/) {
            node.components.add<TerrainComponent>();
        },

        .serialize = [](const void* ptr, const Node& /*node*/, nlohmann::json& out,
                        const Project* /*proj*/) {
            const auto* terrain = static_cast<const TerrainComponent*>(ptr);
            out["terrain_path"] = terrain->terrain_path;
        },

        .deserialize = [](Node& node, const nlohmann::json& j, ResourceManager& res,
                          const Project* proj) {
            auto& terrain = node.components.add<TerrainComponent>();
            terrain.terrain_path = j.value("terrain_path", std::string{});
            if (terrain.terrain_path.empty()) return;

            auto handle = res.load_terrain_asset(
                base_path::resolve_asset(terrain.terrain_path, proj));
            if (!handle) {
                FJELL_CORE_ERROR("terrain: {}", handle.error());
                return;
            }
            terrain.terrain_handle = *handle;
            terrain.terrain = res.get(*handle);
        },

        .script_type_name = "TerrainComponent",
        .script_get = [](Node& node) -> void* { return node.components.get<TerrainComponent>(); },
        .script_add = [](Node& node) -> void* { return &node.components.add<TerrainComponent>(); },
    });
    return true;
}();

} // namespace fjell
