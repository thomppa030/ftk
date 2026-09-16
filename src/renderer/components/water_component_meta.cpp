#include "core/base_path.hpp"
#include "core/log.hpp"
#include "core/project_manager.hpp"
#include "renderer/components/water_component.hpp"
#include "renderer/resources/fjwater_io.hpp"
#include "renderer/resources/material.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "ui/asset_ref_widget.hpp"
#include "ui/inspector_widgets.hpp"
#include "scene/component_registry.hpp"
#include "scene/node.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>

namespace fjell {

namespace {

// Resolve the component's material path to a loaded material, or clear it.
void load_water_material(WaterComponent& water, ResourceManager* resources,
                         const Project* project) {
    water.material_handle = {};
    water.material = nullptr;
    if (water.material_path.empty() || resources == nullptr) return;
    auto resolved = base_path::resolve_asset(water.material_path, project);
    auto handle = resources->load_material_asset(resolved, project);
    if (!handle) {
        FJELL_CORE_ERROR("water: {}", handle.error());
        return;
    }
    water.material_handle = *handle;
    water.material = resources->get(*handle);
}

// Read the .fjwater the component points at. A path that does not load leaves
// the field empty, which is flat water rather than a missing surface.
void load_water_field(WaterComponent& water, const Project* project) {
    water.water_loaded = false;
    water.water = {};
    if (water.water_path.empty()) return;
    auto loaded = load_fjwater_file(base_path::resolve_asset(water.water_path, project));
    if (!loaded) {
        FJELL_CORE_ERROR("water: {}", loaded.error());
        return;
    }
    water.water = std::move(*loaded);
    water.water_loaded = true;
}

const bool registered = [] {
    ComponentRegistry::instance().register_component<WaterComponent>({
        .display_name = "Water",
        .json_key = "water",
        .color = {0.30f, 0.55f, 0.75f, 1.0f},

        .draw = [](Node3D& /*node*/, void* ptr, const InspectorContext& ctx) -> bool {
            auto* water = static_cast<WaterComponent*>(ptr);
            bool changed = false;

            Project* project = ctx.project_mgr != nullptr ? ctx.project_mgr->active() : nullptr;

            static const std::string water_exts[] = {".fjwater"};
            if (asset_ref_field({.label = "Wave field",
                                 .id = "##WaterRef",
                                 .extensions = water_exts,
                                 .ctx = &ctx},
                                water->water_path)) {
                load_water_field(*water, project);
                changed = true;
            }

            static const std::string mat_exts[] = {".fjmat"};
            if (asset_ref_field({.label = "Material",
                                 .id = "##WaterMatRef",
                                 .extensions = mat_exts,
                                 .ctx = &ctx},
                                water->material_path)) {
                load_water_material(*water, ctx.resource_manager, project);
                changed = true;
            }

            ImGui::Separator();

            if (ImGui::DragFloat("Size", &water->size, 1.0f, 1.0f, 100000.0f, "%.0f m")) {
                water->size = std::max(water->size, 1.0f);
                changed = true;
            }

            int patches = static_cast<int>(water->patches_per_side);
            if (ImGui::DragInt("Patches", &patches, 1.0f, 1, 512)) {
                water->patches_per_side = static_cast<std::uint32_t>(std::clamp(patches, 1, 512));
                changed = true;
            }

            // What the tessellation actually comes to, against what the wave
            // field needs. A train under about four vertices per wavelength
            // cannot be drawn and reads as noise instead of chop, and nothing
            // else in the editor puts those two numbers side by side.
            const float spacing = water->vertex_spacing();
            ImGui::TextDisabled("%.2f m between vertices", spacing);
            if (water->water_loaded && !water->water.waves.empty()) {
                float shortest = water->water.waves.front().wavelength;
                for (const auto& wave : water->water.waves) {
                    shortest = std::min(shortest, wave.wavelength);
                }
                const float per_wave = shortest / std::max(spacing, 1e-4f);
                if (per_wave < 4.0f) {
                    ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.25f, 1.0f),
                                       "shortest wave has %.1f vertices — needs 4", per_wave);
                    ImGui::SetItemTooltip("Raise Patches, shrink Size, or drop the "
                                          "shortest trains from the wave field.");
                } else {
                    ImGui::TextDisabled("shortest wave has %.1f vertices", per_wave);
                }
            }

            const std::uint32_t verts = (water->patches_per_side * 11 + 1) *
                                        (water->patches_per_side * 11 + 1);
            ImGui::TextDisabled("%u vertices", verts);

            return changed;
        },

        .init = [](Node3D& node, const InspectorContext& /*ctx*/) {
            node.components.add<WaterComponent>();
        },

        .serialize = [](const void* ptr, const Node& /*node*/, nlohmann::json& out,
                        const Project* proj) {
            const auto* water = static_cast<const WaterComponent*>(ptr);
            if (!water->water_path.empty()) {
                out["water_path"] = base_path::asset_reference(water->water_path, proj);
            }
            if (!water->material_path.empty()) {
                out["material"] = base_path::asset_reference(water->material_path, proj);
            }
            out["size"] = water->size;
            out["patches_per_side"] = water->patches_per_side;
        },

        .deserialize = [](Node& node, const nlohmann::json& j, ResourceManager& res,
                          const Project* proj) {
            auto& water = node.components.add<WaterComponent>();
            water.water_path = j.value("water_path", std::string{});
            water.material_path = j.value("material", std::string{});
            water.size = j.value("size", 200.0f);
            water.patches_per_side = j.value("patches_per_side", 24u);
            load_water_material(water, &res, proj);
            load_water_field(water, proj);
        },

        .material = [](void* ptr) -> Material* {
            return static_cast<WaterComponent*>(ptr)->material;
        },
        .script_type_name = "WaterComponent",
        .script_get = [](Node& node) -> void* { return node.components.get<WaterComponent>(); },
        .script_add = [](Node& node) -> void* { return &node.components.add<WaterComponent>(); },
    });
    return true;
}();

} // namespace
} // namespace fjell
