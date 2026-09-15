#include "core/asset_type_registry.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"
#include "renderer/components/terrain_component.hpp"
#include "core/project_manager.hpp"
#include "renderer/resources/material.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/resources/terrain_asset.hpp"
#include "ui/icons_lc.hpp"
#include "ui/inspector_widgets.hpp"
#include "ui/terrain_sculpt.hpp"
#include "ui/asset_ref_widget.hpp"
#include "scene/component_registry.hpp"
#include "scene/node.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

namespace fjell {

namespace {

// Resolve the component's material path to a loaded material, or clear it.
void load_terrain_material(TerrainComponent& terrain, ResourceManager* resources,
                           const Project* project) {
    terrain.material_handle = {};
    terrain.material = nullptr;
    if (terrain.material_path.empty() || resources == nullptr) return;
    auto resolved = base_path::resolve_asset(terrain.material_path, project);
    auto handle = resources->load_material_asset(resolved, project);
    if (!handle) {
        FJELL_CORE_ERROR("terrain material: {}", handle.error());
        return;
    }
    terrain.material_handle = *handle;
    terrain.material = resources->get(*handle);
}

// The layer table: one .fjmat and a tiling per layer. Layer 0 is the ground
// where nothing else has been painted or blended in.
void draw_layers(TerrainAsset& asset, const InspectorContext& ctx, const Project* project) {
    ImGui::Separator();
    ImGui::Text("Layers");

    static const std::string material_exts[] = {".fjmat"};
    const auto* mat_type = AssetTypeRegistry::instance().find_by_name("Material");
    // Stable ids and labels for each row, so the widgets keep their state
    // across frames without building strings every draw.
    static constexpr const char* labels[FJTERRAIN_MAX_LAYERS] = {"Layer 0", "Layer 1",
                                                                 "Layer 2", "Layer 3"};
    static constexpr const char* ids[FJTERRAIN_MAX_LAYERS] = {
        "##TerrainLayer0", "##TerrainLayer1", "##TerrainLayer2", "##TerrainLayer3"};
    static constexpr const char* tiling_ids[FJTERRAIN_MAX_LAYERS] = {
        "##TerrainTiling0", "##TerrainTiling1", "##TerrainTiling2", "##TerrainTiling3"};

    for (uint32_t i = 0; i < asset.layer_count(); ++i) {
        std::string path = asset.layer_material_path(i);
        if (asset_ref_field({.label = labels[i], .id = ids[i], .extensions = material_exts,
                             .ctx = &ctx, .asset_type = mat_type},
                            path)) {
            asset.set_layer_material_path(i, path);
            if (ctx.resource_manager != nullptr) {
                ctx.resource_manager->resolve_terrain_layers(asset, project);
            }
        }
        float tiling = asset.layer_uv_scale(i);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::DragFloat(tiling_ids[i], &tiling, 0.1F, 0.1F, 1000.0F, "Tiling %.1f m")) {
            asset.set_layer_uv_scale(i, tiling);
        }
    }

    if (asset.layer_count() < FJTERRAIN_MAX_LAYERS) {
        if (ImGui::Button("Add Layer")) {
            asset.set_layer_count(asset.layer_count() + 1);
        }
        if (asset.layer_count() > 0) ImGui::SameLine();
    }
    if (asset.layer_count() > 0 && ImGui::Button("Remove Last Layer")) {
        asset.set_layer_count(asset.layer_count() - 1);
    }
}

} // namespace

static const bool registered = [] {
    ComponentRegistry::instance().register_component<TerrainComponent>({
        .display_name = "Terrain",
        .json_key = "terrain",
        .color = {0.35f, 0.65f, 0.35f, 1.0f},

        .draw = [](Node3D& /*node*/, void* ptr, const InspectorContext& ctx) -> bool {
            auto* terrain = static_cast<TerrainComponent*>(ptr);
            bool changed = false;

            Project* project = ctx.project_mgr != nullptr ? ctx.project_mgr->active() : nullptr;

            static const std::string terrain_exts[] = {".fjterrain"};
            if (asset_ref_field({.label = "Terrain Asset", .id = "##TerrainAsset",
                                 .extensions = terrain_exts, .ctx = &ctx},
                                terrain->terrain_path)) {
                // Resolve immediately: the path alone renders nothing, and the
                // inspector is the one place that changes it outside load.
                terrain->terrain_handle = {};
                terrain->terrain = nullptr;
                if (!terrain->terrain_path.empty() && ctx.resource_manager != nullptr) {
                    auto resolved = base_path::resolve_asset(terrain->terrain_path, project);
                    auto handle = ctx.resource_manager->load_terrain_asset(resolved, project);
                    if (handle) {
                        terrain->terrain_handle = *handle;
                        terrain->terrain = ctx.resource_manager->get(*handle);
                    } else {
                        FJELL_CORE_ERROR("terrain: {}", handle.error());
                    }
                }
                changed = true;
            }

            // The surface shader. Empty is the engine's terrain.fjsl at its
            // defaults; assign a .fjmat to tune or replace the blending.
            static const std::string material_exts[] = {".fjmat"};
            const auto* mat_type = AssetTypeRegistry::instance().find_by_name("Material");
            if (asset_ref_field({.label = "Material", .id = "##TerrainMaterial",
                                 .extensions = material_exts, .ctx = &ctx,
                                 .asset_type = mat_type},
                                terrain->material_path)) {
                load_terrain_material(*terrain, ctx.resource_manager, project);
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

            // Layers live in the .fjterrain, so edits mark the asset dirty
            // rather than the node; the scene save writes the asset too.
            if (terrain->has_asset()) {
                draw_layers(*terrain->terrain, ctx, project);
            }

            // Brush settings are editor state on the tool, not data on the
            // component, so touching them does not mark the node changed.
            if (ctx.sculpt != nullptr && terrain->has_asset()) {
                ImGui::Separator();
                ImGui::Text("Sculpt");
                auto& tool = *ctx.sculpt;
                using BrushTool = TerrainSculptTool::BrushTool;
                struct BrushEntry {
                    BrushTool tool;
                    const char* id;
                    const char* label;
                    const char* tip;
                };
                static constexpr BrushEntry brushes[] = {
                    {BrushTool::raise,   "brush_raise",   "Raise " ICON_LC_ARROW_UP_FROM_LINE,
                     "Raise the ground under the brush"},
                    {BrushTool::lower,   "brush_lower",   "Lower " ICON_LC_ARROW_DOWN_TO_LINE,
                     "Lower the ground under the brush"},
                    {BrushTool::smooth,  "brush_smooth",  "Smooth " ICON_LC_BLEND,
                     "Blend each point toward its neighbours"},
                    {BrushTool::flatten, "brush_flatten", "Flatten " ICON_LC_CHEVRONS_DOWN_UP,
                     "Pull the ground to the height where the stroke began"},
                    {BrushTool::noise,   "brush_noise",   "Noise " ICON_LC_AUDIO_WAVEFORM,
                     "Roughen the ground under the brush"},
                };
                // Clicking the active brush puts it down again, so the row
                // needs no separate "none" entry.
                bool first = true;
                for (const auto& entry : brushes) {
                    if (!first) ImGui::SameLine();
                    first = false;
                    const bool on = tool.tool() == entry.tool;
                    if (toggle_button(entry.id, entry.label, on, entry.tip)) {
                        tool.set_tool(on ? BrushTool::none : entry.tool);
                    }
                }
                if (tool.active()) {
                    auto& brush = tool.brush();
                    const bool rate_brush =
                        tool.tool() == BrushTool::smooth || tool.tool() == BrushTool::flatten;
                    ImGui::SliderFloat("Radius (m)", &brush.radius, 0.5f, 100.0f, "%.1f",
                                       ImGuiSliderFlags_Logarithmic);
                    if (rate_brush) {
                        ImGui::SliderFloat("Rate (1/s)", &brush.rate, 0.1f, 20.0f, "%.1f",
                                           ImGuiSliderFlags_Logarithmic);
                    } else {
                        ImGui::SliderFloat("Strength (m/s)", &brush.strength, 0.1f, 50.0f, "%.1f",
                                           ImGuiSliderFlags_Logarithmic);
                    }
                    ImGui::SliderFloat("Hardness", &brush.hardness, 0.0f, 1.0f, "%.2f");
                    ImGui::TextDisabled("Drag on the terrain to sculpt. Ctrl+Z undoes a stroke.");
                }
            }
            return changed;
        },

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
