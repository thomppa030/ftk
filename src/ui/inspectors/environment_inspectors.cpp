#include "ui/inspectors/environment_inspectors.hpp"

#include "core/asset_type_registry.hpp"
#include "core/log.hpp"
#include "core/project.hpp"
#include "renderer/components/motion_source_component.hpp"
#include "renderer/components/terrain_component.hpp"
#include "renderer/components/water_component.hpp"
#include "renderer/resources/asset_shader_type.hpp"
#include "renderer/resources/material_asset.hpp"
#include "renderer/resources/terrain_asset.hpp"
#include "renderer/resources/water_clipmap.hpp"
#include "renderer/resources/water_surface.hpp"
#include "scene/node.hpp"
#include "ui/asset_ref_widget.hpp"
#include "ui/brush_widgets.hpp"
#include "ui/inspectors/asset_rows.hpp"
#include "ui/kit/button.hpp"
#include "ui/kit/choice.hpp"
#include "ui/kit/feedback.hpp"

#include <cmath>
#include "ui/kit/field.hpp"
#include "ui/kit/icons.hpp"
#include "ui/kit/list_editor.hpp"
#include "ui/kit/row.hpp"
#include "ui/kit/section.hpp"
#include "ui/terrain_sculpt.hpp"
#include "ui/theme.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace fjell {

namespace {

using BrushTool = TerrainSculptTool::BrushTool;

// The terrain brush's tools, in the order the toolbar shows them.
struct Tool {
    BrushTool tool;
    const char* id;
    const char* icon;
    const char* name;
    const char* tip;
};
constexpr Tool TOOLS[] = {
    {BrushTool::raise, "raise", ui::icon::raise, "Raise", "Raise: lift the ground under the brush"},
    {BrushTool::lower, "lower", ui::icon::lower, "Lower", "Lower: sink the ground under the brush"},
    {BrushTool::smooth, "smooth", ui::icon::smooth, "Smooth", "Smooth: blend each point toward its neighbours"},
    {BrushTool::flatten, "flatten", ui::icon::flatten, "Flatten",
     "Flatten: pull the ground to the height where the stroke began"},
    {BrushTool::noise, "noise", ui::icon::noise, "Noise", "Noise: roughen the ground under the brush"},
    {BrushTool::paint, "paint", ui::icon::paint, "Paint", "Paint: lay a material layer onto the ground"},
};

constexpr ui::Choice<WaterComponent::Extent> EXTENTS[] = {
    {WaterComponent::Extent::bounded, "Bounded"},
    {WaterComponent::Extent::infinite, "Infinite"},
};

// Writes a fresh .fjmat on the default terrain shader beside the .fjterrain
// and registers it with the project, returning its project-relative path.
std::optional<std::string> create_terrain_material(const TerrainComponent& terrain, Project& project) {
    namespace fs = std::filesystem;
    const fs::path terrain_abs = base_path::resolve_asset(terrain.terrain_path, &project);
    if (terrain_abs.empty()) return std::nullopt;
    fs::path material_abs = terrain_abs;
    material_abs.replace_extension(".fjmat");

    std::error_code ec;
    if (fs::exists(material_abs, ec)) {
        FJELL_CORE_ERROR("terrain material: {} already exists", material_abs.string());
        return std::nullopt;
    }
    MaterialAsset asset;
    asset.shader = "shaders/terrain.fjsl";
    try {
        save_material_asset(material_abs.string(), asset);
    } catch (const std::exception& e) {
        FJELL_CORE_ERROR("terrain material: {}", e.what());
        return std::nullopt;
    }

    const fs::path relative = fs::relative(material_abs, project.root(), ec);
    const std::string relative_str = ec ? material_abs.generic_string() : relative.generic_string();
    if (auto* db = project.asset_db()) {
        db->register_asset(relative_str);
    }
    return relative_str;
}

// The layer table: one .fjmat, a tiling and a surface per layer. Each layer
// is a channel of the painted weights, so layers are added and taken off at
// the end and never reordered. They live in the .fjterrain, which the scene
// save writes, not in the component.
void terrain_layers(TerrainAsset& asset, const InspectorContext& ctx) {
    static const std::string material_exts[] = {".fjmat"};
    static const std::string surface_exts[] = {".fjsurface"};
    const auto* mat_type = AssetTypeRegistry::instance().find_by_name("Material");
    const auto* surface_type = AssetTypeRegistry::instance().find_by_name("Surface");
    const Project* project = project_of(ctx);
    const auto resolve = [&] {
        if (ctx.resource_manager != nullptr) ctx.resource_manager->resolve_terrain_layers(asset, project);
    };

    ui::subheading("Layers");
    int& open = *ImGui::GetStateStorage()->GetIntRef(ImGui::GetID("##open_layer"), 0);
    std::optional<int> select;
    ui::ListEditor list("##layers", asset.layer_count(), {.reorder = false, .remove = false, .opens = true});
    for (uint32_t i = 0; i < asset.layer_count(); ++i) {
        const bool is_open = open == static_cast<int>(i);
        if (!list.begin_item(i, is_open)) continue;
        std::string material = asset.layer_material_path(i);
        if (is_open) {
            if (auto t = ui::PropertyTable("##layer")) {
                ui::row("Material", [&] {
                    if (asset_slot({.label = "Material", .id = "##material", .extensions = material_exts,
                                    .ctx = &ctx, .asset_type = mat_type,
                                    .shader_types = only_shader_type(ShaderType::Spatial)},
                                   material)) {
                        asset.set_layer_material_path(i, material);
                        resolve();
                    }
                });
                ui::row("Tiling", "How far one repeat of the layer's material stretches.", [&] {
                    float tiling = asset.layer_uv_scale(i);
                    if (ui::drag("##tiling", tiling, {.speed = 0.1f, .min = 0.1f, .max = 1000.0f,
                                                      .unit = ui::Unit::Metres, .format = "%.1f"})
                            .changed) {
                        asset.set_layer_uv_scale(i, tiling);
                    }
                });
                ui::row("Surface", "What physics reports for ground this layer covers most.", [&] {
                    std::string surface = asset.layer_surface_path(i);
                    if (asset_slot({.label = "Surface", .id = "##surface", .extensions = surface_exts,
                                    .ctx = &ctx, .asset_type = surface_type},
                                   surface)) {
                        asset.set_layer_surface_path(i, surface);
                        resolve();
                    }
                });
            }
        } else {
            const std::string name = material.empty()
                ? std::string("None")
                : std::filesystem::path(base_path::asset_display_path(material, project)).filename().string();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(name.c_str());
        }
        list.end_item();
        if (list.clicked()) select = is_open ? -1 : static_cast<int>(i);
    }
    if (asset.layer_count() == 0) list.empty("No layers. The ground draws with the terrain material alone");
    if (asset.layer_count() < FJTERRAIN_MAX_LAYERS) {
        if (list.add_button("Add layer")) {
            asset.set_layer_count(asset.layer_count() + 1);
            open = static_cast<int>(asset.layer_count()) - 1;
        }
    }
    if (asset.layer_count() > 0) {
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        if (ui::action(ui::icon::remove, "Remove last layer", ui::ButtonKind::GhostDanger)) {
            asset.set_layer_count(asset.layer_count() - 1);
        }
        ImGui::SetItemTooltip("Layers are channels of the painted weights, so they come off the end");
    }
    if (select) open = *select;
}

// The sculpt and paint tool the terrain carries. Its settings are the tool's,
// not the scene's, so nothing here is an edit of the scene.
void terrain_brush(TerrainComponent& terrain, TerrainSculptTool& tool) {
    const char* active_name = nullptr;
    for (const auto& t : TOOLS) {
        if (t.tool == tool.tool()) active_name = t.name;
    }
    char heading[48];
    std::snprintf(heading, sizeof(heading), active_name != nullptr ? "Sculpt · %s" : "Sculpt",
                  active_name != nullptr ? active_name : "");
    ui::subheading(heading);

    // A stroke finishes with the tool it began with, so the row is dead while
    // one runs. Clicking the active tool puts it down again.
    ImGui::BeginDisabled(tool.stroking());
    bool first = true;
    for (const auto& t : TOOLS) {
        if (!first) ImGui::SameLine(0.0f, theme::GAP_S);
        first = false;
        const bool on = tool.tool() == t.tool;
        if (ui::tool_button(t.id, t.icon, on, t.tip)) {
            tool.set_tool(on ? BrushTool::none : t.tool);
        }
    }
    ImGui::EndDisabled();
    if (!tool.active()) return;

    const bool painting = tool.tool() == BrushTool::paint;
    const uint32_t layers = terrain.terrain->layer_count();
    if (painting && tool.paint_layer() >= layers && layers > 0) tool.set_paint_layer(0);
    if (auto t = ui::PropertyTable("##brush")) {
        if (painting && layers > 0) {
            ui::row("Layer", [&] {
                std::vector<ui::Choice<uint32_t>> choices;
                std::vector<std::string> names;
                names.reserve(layers);
                for (uint32_t i = 0; i < layers; ++i) {
                    const std::string& path = terrain.terrain->layer_material_path(i);
                    char name[96];
                    std::snprintf(name, sizeof(name), "Layer %u%s%s", i + 1, path.empty() ? "" : " · ",
                                  path.empty() ? "" : std::filesystem::path(path).stem().string().c_str());
                    names.emplace_back(name);
                }
                for (uint32_t i = 0; i < layers; ++i) choices.push_back({i, names[i].c_str()});
                uint32_t layer = tool.paint_layer();
                if (ui::combo("##layer", layer, std::span<const ui::Choice<uint32_t>>(choices)).changed) {
                    tool.set_paint_layer(layer);
                }
            });
        }
        draw_brush_settings(tool.brush().settings());
    }
    if (painting && layers == 0) {
        ui::status(ui::Severity::Warning, "Add a layer above to have something to paint");
    }
    ui::hint(painting ? "Drag on the terrain to paint. Ctrl Z undoes a stroke"
                      : "Drag on the terrain to sculpt. Ctrl Z undoes a stroke");
}

} // namespace

ui::Edit draw_terrain(Node3D& node, void* component, const InspectorContext& ctx) {
    auto& terrain = *static_cast<TerrainComponent*>(component);
    static const std::string terrain_exts[] = {".fjterrain"};
    static const std::string material_exts[] = {".fjmat"};
    const auto* mat_type = AssetTypeRegistry::instance().find_by_name("Material");
    const Project* project = project_of(ctx);
    ui::Edit edit;

    // The node places a terrain and nothing more: its size is the asset's
    // world_size and its shape is a grid sampled along world X and Z, so the
    // renderer, physics and ray tracing all read translation only. Silently
    // ignoring the other two fields is a trap — a scaled node would look
    // right in the transform panel and collide where nothing is drawn.
    {
        const auto& t = node.transform;
        const bool rotated = std::abs(t.rotation.w) < 0.9999F;
        const bool scaled = std::abs(t.scale.x - 1.0F) > 1e-3F ||
                            std::abs(t.scale.y - 1.0F) > 1e-3F ||
                            std::abs(t.scale.z - 1.0F) > 1e-3F;
        if (rotated || scaled) {
            ui::status(ui::Severity::Warning,
                       rotated && scaled
                           ? "Rotation and scale are ignored; a terrain's size is its asset's"
                           : rotated ? "Rotation is ignored; terrain is sampled along world X and Z"
                                     : "Scale is ignored; a terrain's size is its asset's world size");
        }
    }

    if (auto t = ui::PropertyTable("##terrain")) {
        ui::row("Terrain", [&] {
            if (asset_slot({.label = "Terrain", .id = "##terrain", .extensions = terrain_exts, .ctx = &ctx},
                           terrain.terrain_path)) {
                // Resolved at once: the path alone draws nothing.
                terrain.terrain_handle = {};
                terrain.terrain = nullptr;
                if (!terrain.terrain_path.empty() && ctx.resource_manager != nullptr) {
                    auto handle = ctx.resource_manager->load_terrain_asset(
                        base_path::resolve_asset(terrain.terrain_path, project), project);
                    if (handle) {
                        terrain.terrain_handle = *handle;
                        terrain.terrain = ctx.resource_manager->get(*handle);
                    } else {
                        FJELL_CORE_ERROR("terrain: {}", handle.error());
                    }
                }
                edit = {true, true};
            }
        });
        ui::row("Material", "The blending of the layers and the lighting of the ground. Empty draws "
                            "with the engine's terrain shader at its defaults.",
                [&] {
                    if (asset_slot({.label = "Material", .id = "##material", .extensions = material_exts,
                                    .ctx = &ctx, .asset_type = mat_type,
                                    .shader_types = only_shader_type(ShaderType::Terrain)},
                                   terrain.material_path)) {
                        load_terrain_material(terrain, ctx.resource_manager, project);
                        edit = {true, true};
                    }
                });
        if (terrain.material_path.empty()) {
            // The blend rules are material parameters, and parameters live
            // in a .fjmat, so tuning them from here means giving this
            // terrain one. It goes beside the .fjterrain under its name.
            ui::row("", [&] {
                const bool can = !terrain.terrain_path.empty() && ctx.project_mgr != nullptr
                              && ctx.project_mgr->active() != nullptr;
                ImGui::BeginDisabled(!can);
                if (ui::action(ui::icon::add, "Create material")) {
                    if (auto created = create_terrain_material(terrain, *ctx.project_mgr->active())) {
                        terrain.material_path = *created;
                        load_terrain_material(terrain, ctx.resource_manager, project);
                        edit = {true, true};
                    }
                }
                ImGui::EndDisabled();
                ImGui::SetItemTooltip(can ? "Writes a .fjmat on the terrain shader beside the .fjterrain"
                                          : "Needs a terrain asset first");
            });
        }
        if (terrain.terrain != nullptr) {
            const auto& data = terrain.terrain->data();
            ui::row("Samples", [&] {
                char text[96];
                std::snprintf(text, sizeof(text), "%u \xc3\x97 %u, %u patches of %u quads", data.sample_count(),
                              data.sample_count(), terrain.terrain->patches_per_side(), data.patch_quads());
                ui::readout(text);
            });
            ui::row("Size", [&] {
                char text[64];
                std::snprintf(text, sizeof(text), "%.0f m across, %.1f m high", terrain.terrain->world_size(),
                              terrain.terrain->height_scale());
                ui::readout(text);
            });
        }
        ui::row("Wireframe", [&] { edit |= ui::checkbox("##wireframe", terrain.show_wireframe); });
    }
    if (terrain.has_asset()) {
        terrain_layers(*terrain.terrain, ctx);
        if (ctx.sculpt != nullptr) terrain_brush(terrain, *ctx.sculpt);
    }
    return edit;
}

ui::Edit draw_water(Node3D& /*node*/, void* component, const InspectorContext& ctx) {
    auto& water = *static_cast<WaterComponent*>(component);
    static const std::string water_exts[] = {".fjwater"};
    static const std::string mat_exts[] = {".fjmat"};
    const Project* project = project_of(ctx);
    ui::Edit edit;
    const bool bounded = water.extent == WaterComponent::Extent::bounded;
    const float spacing = water.vertex_spacing();
    const float shortest = water.water_loaded ? water::shortest_wavelength(water.water) : 0.0f;
    const float per_wave = shortest > 0.0f ? shortest / std::max(spacing, 1e-4f) : 0.0f;
    if (auto t = ui::PropertyTable("##water")) {
        ui::row("Wave field", [&] {
            if (asset_slot({.label = "Wave field", .id = "##field", .extensions = water_exts, .ctx = &ctx},
                           water.water_path)) {
                load_water_field(water, project);
                edit = {true, true};
            }
        });
        ui::row("Material", [&] {
            if (asset_slot({.label = "Material", .id = "##material", .extensions = mat_exts, .ctx = &ctx,
                            .shader_types = only_shader_type(ShaderType::Water)},
                           water.material_path)) {
                load_water_material(water, ctx.resource_manager, project);
                edit = {true, true};
            }
        });
        ui::row("Medium", "The fog drawn under the surface, seen from below it. Empty is clear water.", [&] {
            if (asset_slot({.label = "Medium", .id = "##medium", .extensions = mat_exts, .ctx = &ctx,
                            .shader_types = only_shader_type(ShaderType::Fog)},
                           water.medium_material_path)) {
                load_water_medium(water, ctx.resource_manager, project);
                edit = {true, true};
            }
        });
        ui::row("Extent", "Bounded is a square centred on the node: a lake, a harbour. Infinite "
                          "follows the camera to the horizon.",
                [&] { edit |= ui::choice("##extent", water.extent, EXTENTS); });
        // Size and tessellation are the placed square's; an infinite sea
        // has neither, and its levels are fixed rather than authored.
        ui::row("Size", [&] {
            ImGui::BeginDisabled(!bounded);
            const ui::Edit e = ui::drag("##size", water.size, {.speed = 1.0f, .min = 1.0f, .max = 100000.0f,
                                                               .unit = ui::Unit::Metres, .format = "%.0f"});
            ImGui::EndDisabled();
            if (!bounded) ImGui::SetItemTooltip("An infinite sea follows the camera and has no size");
            edit |= e;
        });
        ui::row("Patches", "Across one side; more patches draw finer waves.", [&] {
            int patches = static_cast<int>(water.patches_per_side);
            ImGui::BeginDisabled(!bounded);
            const ui::Edit e = ui::drag_int("##patches", patches, 1.0f, 1, 512);
            ImGui::EndDisabled();
            if (!bounded) ImGui::SetItemTooltip("An infinite sea's levels are fixed");
            if (e.changed) water.patches_per_side = static_cast<std::uint32_t>(std::clamp(patches, 1, 512));
            edit |= e;
        });

        // What the tessellation comes to, against what the wave field needs:
        // nothing else in the editor puts those two numbers side by side.
        ui::row("Vertex spacing", [&] {
            char text[64];
            if (bounded) {
                std::snprintf(text, sizeof(text), "%.2f m", spacing);
            } else {
                std::snprintf(text, sizeof(text), "%.2f m at the camera, %.0f m of sea", spacing,
                              WaterClipmap::half_extent());
            }
            ui::readout(text);
        });
        const std::uint32_t verts = bounded
            ? (water.patches_per_side * WATER_PATCH_QUADS + 1) * (water.patches_per_side * WATER_PATCH_QUADS + 1)
            : WaterClipmap::patch_count() * (WATER_PATCH_QUADS + 1) * (WATER_PATCH_QUADS + 1);
        ui::row("Vertices", [&] {
            char text[32];
            std::snprintf(text, sizeof(text), "%u", verts);
            ui::readout(text);
        });
        if (per_wave > 0.0f) {
            ui::row("Shortest wave", [&] {
                char text[48];
                std::snprintf(text, sizeof(text), "%.1f vertices", per_wave);
                ui::readout(text);
            });
        }
    }
    // A train under about four vertices per wavelength can't be drawn and
    // reads as noise instead of chop.
    if (per_wave > 0.0f && per_wave < 4.0f) {
        ui::status(ui::Severity::Warning, "The shortest wave needs 4 vertices to be drawn");
        ui::hint("Raise Patches, shrink Size, or drop the shortest trains from the wave field");
    }
    return edit;
}

ui::Edit draw_motion_source(Node3D& /*node*/, void* component, const InspectorContext& /*ctx*/) {
    auto& ms = *static_cast<MotionSourceComponent*>(component);
    ui::Edit edit;
    if (auto t = ui::PropertyTable("##motion_source")) {
        ui::row("Radius", "How wide a mark this leaves. 0 follows the node's own size.", [&] {
            edit |= ui::drag("##radius", ms.radius,
                             {.speed = 0.05f, .min = 0.0f, .max = 20.0f, .unit = ui::Unit::Metres});
        });
        ui::row("Height", "How high the movement reaches, so fog above a walker's head is left alone. "
                          "0 follows the node's own size.",
                [&] {
                    edit |= ui::drag("##height", ms.height,
                                     {.speed = 0.05f, .min = 0.0f, .max = 20.0f, .unit = ui::Unit::Metres});
                });
        ui::row("Strength", [&] {
            edit |= ui::drag("##strength", ms.strength, {.speed = 0.01f, .min = 0.0f, .max = 4.0f});
        });
    }
    return edit;
}

} // namespace fjell
