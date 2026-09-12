#include "scene/component_registry.hpp"
#include "core/base_path.hpp"
#include "renderer/components/tilemap_component.hpp"
#include "scene/node.hpp"
#include "ui/asset_ref_widget.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

namespace fjell {

static const bool registered = [] {
    ComponentRegistry::instance().register_component<TilemapComponent>({
        .display_name = "Tilemap",
        .json_key = "tilemap",
        .color = {0.30f, 0.65f, 0.85f, 1.0f},

        .draw = [](Node3D& /*node*/, void* ptr, const InspectorContext& ctx) -> bool {
            auto* tm = static_cast<TilemapComponent*>(ptr);
            bool changed = false;
            static const std::string exts[] = {".png", ".jpg", ".jpeg"};
            if (asset_ref_field({.label = "Tileset", .id = "##TilemapTileset", .extensions = exts, .ctx = &ctx}, tm->tileset_texture)) { tm->texture_loaded = false; tm->dirty = true; changed = true; }
            ImGui::Text("Tile Size (px)"); ImGui::SetNextItemWidth(-1);
            if (ImGui::DragInt2("##TileTileSize", &tm->tile_size.x, 1, 1, 512)) { tm->dirty = true; changed = true; }
            ImGui::Text("Map Size (tiles)"); ImGui::SetNextItemWidth(-1);
            if (ImGui::DragInt2("##TileMapSize", &tm->map_size.x, 1, 1, 256)) { tm->tiles.resize(tm->map_size.x * tm->map_size.y, -1); tm->dirty = true; changed = true; }
            ImGui::Text("World Tile Size"); ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat("##TileWorldSize", &tm->world_tile_size, 0.01f, 0.01f, 10.0f)) { tm->dirty = true; changed = true; }
            ImGui::Text("Sort Layer"); ImGui::SetNextItemWidth(-1);
            if (ImGui::DragInt("##TileSortLayer", &tm->sort_layer, 1.0f)) changed = true;
            if (ImGui::Button("Fill All (tile 0)")) { tm->tiles.assign(tm->map_size.x * tm->map_size.y, 0); tm->dirty = true; changed = true; }

            ImGui::Separator();
            if (ImGui::Checkbox("Lit (PBR + GI)", &tm->lit)) changed = true;
            if (tm->lit) {
                if (asset_ref_field({.label = "Normal Map", .id = "##TileNormal", .extensions = exts, .ctx = &ctx}, tm->normal_path)) { tm->normal_loaded = false; changed = true; }
                ImGui::Text("Roughness"); ImGui::SetNextItemWidth(-1);
                if (ImGui::DragFloat("##TileRoughness", &tm->roughness, 0.01f, 0.0f, 1.0f)) changed = true;
                ImGui::Text("Metallic"); ImGui::SetNextItemWidth(-1);
                if (ImGui::DragFloat("##TileMetallic", &tm->metallic, 0.01f, 0.0f, 1.0f)) changed = true;
                ImGui::Text("Emission"); ImGui::SetNextItemWidth(-1);
                if (ImGui::DragFloat("##TileEmission", &tm->emission, 0.05f, 0.0f, 100.0f)) changed = true;
            }
            return changed;
        },

        .init = [](Node3D& node, const InspectorContext& /*ctx*/) {
            auto& tm = node.components.add<TilemapComponent>();
            tm.tiles.resize(tm.map_size.x * tm.map_size.y, -1);
        },

        .serialize = [](const void* ptr, const Node& /*node*/, nlohmann::json& out,
                        const Project* proj) {
            auto* tm = static_cast<const TilemapComponent*>(ptr);
            out["tileset"] = base_path::asset_reference(tm->tileset_texture, proj);
            out["tile_size"] = {tm->tile_size.x, tm->tile_size.y};
            out["map_size"] = {tm->map_size.x, tm->map_size.y};
            out["world_tile_size"] = tm->world_tile_size;
            out["sort_layer"] = tm->sort_layer;
            if (tm->pixel_art != true) out["pixel_art"] = tm->pixel_art;
            if (!tm->lit) out["lit"] = false;  // default true for tilemaps
            if (!tm->normal_path.empty()) out["normal"] = base_path::asset_reference(tm->normal_path, proj);
            if (tm->roughness != 1.0f) out["roughness"] = tm->roughness;
            if (tm->metallic != 0.0f) out["metallic"] = tm->metallic;
            if (tm->emission != 0.0f) out["emission"] = tm->emission;
            out["tiles"] = tm->tiles;
        },

        .deserialize = [](Node& node, const nlohmann::json& j, ResourceManager& /*res*/, const Project* /*proj*/) {
            auto& tm = node.components.add<TilemapComponent>();
            tm.tileset_texture = j.value("tileset", "");
            if (j.contains("tile_size")) { auto& ts = j["tile_size"]; tm.tile_size = {ts[0].get<int>(), ts[1].get<int>()}; }
            if (j.contains("map_size")) { auto& ms = j["map_size"]; tm.map_size = {ms[0].get<int>(), ms[1].get<int>()}; }
            tm.world_tile_size = j.value("world_tile_size", 1.0f);
            tm.sort_layer = j.value("sort_layer", -100);
            tm.pixel_art = j.value("pixel_art", true);
            tm.lit = j.value("lit", true);
            tm.normal_path = j.value("normal", "");
            tm.roughness = j.value("roughness", 1.0f);
            tm.metallic = j.value("metallic", 0.0f);
            tm.emission = j.value("emission", 0.0f);
            if (j.contains("tiles")) tm.tiles = j["tiles"].get<std::vector<int32_t>>();
        },

        .script_type_name = {},
    });
    return true;
}();

} // namespace fjell
