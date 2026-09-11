#include "core/base_path.hpp"
#include "core/log.hpp"
#include "renderer/components/terrain_component.hpp"
#include "renderer/resources/resource_manager.hpp"
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

        .draw = [](Node3D& /*node*/, void* ptr, const InspectorContext& /*ctx*/) -> bool {
            auto* terrain = static_cast<TerrainComponent*>(ptr);
            bool changed = false;

            ImGui::Text("Terrain Asset");
            ImGui::SetNextItemWidth(-1);
            if (terrain->terrain_path.empty()) {
                ImGui::TextDisabled("None — assign a .fjterrain");
            } else {
                ImGui::TextUnformatted(terrain->terrain_path.c_str());
            }

            if (terrain->has_asset()) {
                ImGui::Separator();
                ImGui::TextDisabled("Read-only until the terrain editor lands.");
            }

            if (ImGui::Checkbox("Show Wireframe", &terrain->show_wireframe)) changed = true;
            return changed;
        },

        .init = [](Node3D& node, const InspectorContext& /*ctx*/) {
            node.components.add<TerrainComponent>();
        },

        .serialize = [](const void* ptr, const Node& /*node*/, nlohmann::json& out) {
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
