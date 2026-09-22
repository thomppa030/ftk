#include "core/base_path.hpp"
#include "core/log.hpp"
#include "core/project_manager.hpp"
#include "renderer/components/water_component.hpp"
#include "renderer/resources/fjwater_io.hpp"
#include "renderer/resources/water_clipmap.hpp"
#include "renderer/resources/water_surface.hpp"
#include "renderer/resources/material.hpp"
#include "renderer/resources/material_instance.hpp"
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
    // A script's override clones the material it was made from, so it cannot
    // outlive a change of material.
    water.instance.reset();
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

// Resolve the medium material, or clear it. Bound to nothing rather than to
// the wrong kind of shader: the fog pass would build no pipeline from a
// spatial material, and the log says why once.
void load_water_medium(WaterComponent& water, ResourceManager* resources,
                       const Project* project) {
    water.medium_material_handle = {};
    water.medium_material = nullptr;
    if (water.medium_material_path.empty() || resources == nullptr) return;
    auto resolved = base_path::resolve_asset(water.medium_material_path, project);
    auto handle = resources->load_material_asset(resolved, project);
    if (!handle) {
        FJELL_CORE_ERROR("water medium: {}", handle.error());
        return;
    }
    Material* material = resources->get(*handle);
    if (material != nullptr && material->shader_type() != ShaderType::Fog) {
        FJELL_CORE_ERROR("water medium: '{}' is shader_type '{}', a water's medium needs "
                         "shader_type fog; the water has no medium",
                         water.medium_material_path, shader_type_name(material->shader_type()));
        return;
    }
    water.medium_material_handle = *handle;
    water.medium_material = material;
}

// Read the .fjwater the component points at. A path that does not load leaves
// the field empty, which is flat water rather than a missing surface.
void load_water_field(WaterComponent& water, const Project* project) {
    water.water_loaded = false;
    water.water = {};
    if (!water.water_path.empty()) {
        auto loaded = load_fjwater_file(base_path::resolve_asset(water.water_path, project));
        if (loaded) {
            water.water = std::move(*loaded);
            water.water_loaded = true;
        } else {
            FJELL_CORE_ERROR("water: {}", loaded.error());
        }
    }
    // What the renderer and physics read, derived here in still air so a body
    // of water has the sea it was authored with from the first frame — its
    // trains prepared and its simulated sea drawn, which is what the
    // evaluators require. A scene with weather derives it again every tick;
    // one without never does, and this is the field it keeps.
    water.effective_water = water::derive_effective(water.water, {0.0f, 0.0f});
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
            if (asset_ref_field({.label = "Medium",
                                 .id = "##WaterMediumRef",
                                 .extensions = mat_exts,
                                 .ctx = &ctx},
                                water->medium_material_path)) {
                load_water_medium(*water, ctx.resource_manager, project);
                changed = true;
            }

            ImGui::Separator();

            const char* const extents[] = {"Bounded", "Infinite"};
            int extent = static_cast<int>(water->extent);
            if (ImGui::Combo("Extent", &extent, extents, 2)) {
                water->extent = static_cast<WaterComponent::Extent>(extent);
                changed = true;
            }
            ImGui::SetItemTooltip("Bounded is a square centred on the node — a lake, a "
                                  "harbour. Infinite follows the camera to the horizon.");

            const bool bounded = water->extent == WaterComponent::Extent::bounded;

            // Size and tessellation are the placed square's; an infinite sea
            // has neither, and its levels are fixed rather than authored.
            ImGui::BeginDisabled(!bounded);
            if (ImGui::DragFloat("Size", &water->size, 1.0f, 1.0f, 100000.0f, "%.0f m")) {
                water->size = std::max(water->size, 1.0f);
                changed = true;
            }

            int patches = static_cast<int>(water->patches_per_side);
            if (ImGui::DragInt("Patches", &patches, 1.0f, 1, 512)) {
                water->patches_per_side = static_cast<std::uint32_t>(std::clamp(patches, 1, 512));
                changed = true;
            }
            ImGui::EndDisabled();

            // What the tessellation actually comes to, against what the wave
            // field needs. A train under about four vertices per wavelength
            // cannot be drawn and reads as noise instead of chop, and nothing
            // else in the editor puts those two numbers side by side.
            const float spacing = water->vertex_spacing();
            if (bounded) {
                ImGui::TextDisabled("%.2f m between vertices", spacing);
            } else {
                ImGui::TextDisabled("%.2f m between vertices at the camera, %.0f m of sea",
                                    spacing, WaterClipmap::half_extent());
            }
            const float shortest = water::shortest_wavelength(water->water);
            if (water->water_loaded && shortest > 0.0f) {
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

            const std::uint32_t verts =
                bounded ? (water->patches_per_side * WATER_PATCH_QUADS + 1) *
                              (water->patches_per_side * WATER_PATCH_QUADS + 1)
                        : WaterClipmap::patch_count() * (WATER_PATCH_QUADS + 1) *
                              (WATER_PATCH_QUADS + 1);
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
            if (!water->medium_material_path.empty()) {
                out["medium"] = base_path::asset_reference(water->medium_material_path, proj);
            }
            out["size"] = water->size;
            out["patches_per_side"] = water->patches_per_side;
            if (water->extent == WaterComponent::Extent::infinite) {
                out["extent"] = "infinite";
            }
        },

        .deserialize = [](Node& node, const nlohmann::json& j, ResourceManager& res,
                          const Project* proj) {
            auto& water = node.components.add<WaterComponent>();
            water.water_path = j.value("water_path", std::string{});
            water.material_path = j.value("material", std::string{});
            water.medium_material_path = j.value("medium", std::string{});
            water.size = j.value("size", 200.0f);
            water.patches_per_side = j.value("patches_per_side", 24u);
            water.extent = j.value("extent", std::string{}) == "infinite"
                               ? WaterComponent::Extent::infinite
                               : WaterComponent::Extent::bounded;
            load_water_material(water, &res, proj);
            load_water_medium(water, &res, proj);
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
