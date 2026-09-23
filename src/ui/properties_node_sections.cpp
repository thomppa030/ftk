// The Properties panel's sections for what a node is: its transform and the
// settings of its type (camera, light, fog, world environment). They come
// before its components, which are drawn in properties_panel.cpp.

#include "ui/properties_panel.hpp"

#include "core/command_history.hpp"
#include "core/commands.hpp"
#include "core/project_manager.hpp"
#include "environment/environment_resolver.hpp"
#include "environment/environment_state.hpp"
#include "environment/environment_subsystem.hpp"
#include "environment/weather_asset.hpp"
#include "renderer/pass/grid_pass.hpp"
#include "renderer/pass/sky_pass.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/resources/asset_shader_type.hpp"
#include "renderer/resources/material.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/uniform_types.hpp"
#include "scene/camera_node.hpp"
#include "scene/component_meta.hpp"
#include "scene/light_node.hpp"
#include "scene/node_serializer.hpp"
#include "scene/scene.hpp"
#include "core/string_utils.hpp"
#include "ui/asset_ref_widget.hpp"
#include "ui/kit/button.hpp"
#include "ui/kit/choice.hpp"
#include "ui/kit/color_field.hpp"
#include "ui/kit/field.hpp"
#include "ui/kit/icons.hpp"
#include "ui/kit/inset_group.hpp"
#include "ui/kit/row.hpp"
#include "ui/kit/section.hpp"
#include "ui/kit/text_field.hpp"
#include "ui/material_params_widget.hpp"
#include "ui/inspector_widgets.hpp"
#include "ui/theme.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <span>
#include <string>

namespace fjell {

namespace {

glm::vec3 snap_to_grid(const glm::vec3& pos, float cell_size) {
    return {
        std::round(pos.x / cell_size) * cell_size,
        std::round(pos.y / cell_size) * cell_size,
        std::round(pos.z / cell_size) * cell_size,
    };
}

// One uniform's value as a node's own: the field, and after it, while the
// node overrides the value, a button that goes back to the value underneath.
// Returns whether the value changed; `revert` is set when going back.
bool override_row(const FjslUniform& u, std::span<float> values, bool overridden,
                  const char* revert_tip, bool& revert) {
    bool changed = false;
    const std::string label = display_name(u.name);
    ImGui::PushID(u.name.c_str());
    ui::row(label.c_str(), [&] {
        const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
        const float revert_w = overridden ? ImGui::GetFrameHeight() + spacing : 0.0f;
        ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - revert_w, 1.0f));
        changed = draw_uniform_value(u, values);
        if (overridden) {
            ImGui::SameLine(0.0f, spacing);
            revert = ui::icon_button("##revert", ui::icon::revert, revert_tip);
        }
    });
    ImGui::PopID();
    return changed;
}

// The sky shader's uniforms as rows on the World Environment node: each shows
// the node's override when it has one and the .fjsl default otherwise, and an
// edit stores an override on the node. A day profile track of the same name
// takes over from the override while the profile is bound. Returns whether
// an override changed.
bool draw_sky_uniforms(WorldEnvironmentNode& env, const InspectorContext& ictx) {
    bool changed = false;
    if (ictx.pipeline_cache == nullptr) return false;
    const Project* project = ictx.project_mgr != nullptr ? ictx.project_mgr->active() : nullptr;
    const uint32_t pid = ictx.pipeline_cache->register_shader(
        canonical_sky_shader_path(env.sky_shader, project));
    if (ictx.pipeline_cache->has_compile_error(pid)) return false;
    const auto& meta = ictx.pipeline_cache->metadata(pid);
    if (meta.shader_type != ShaderType::Sky) return false;

    bool any = false;
    for (const auto& u : meta.uniforms) any |= u.type != UniformType::Sampler2D;
    if (!any) return false;

    auto box = ui::InsetGroup("##sky_values", "Sky shader", ui::icon::shader,
                              theme::category(theme::Category::Environment));
    if (!box) return false;
    std::string revert_name;
    if (auto t = ui::PropertyTable("##sky_uniforms")) {
        for (const auto& u : meta.uniforms) {
            if (u.type == UniformType::Sampler2D) continue;
            const uint32_t count = uniform_float_count(u.type);
            auto override_it = std::find_if(env.sky_uniforms.begin(), env.sky_uniforms.end(),
                                            [&](const WorldEnvironmentNode::SkyUniform& o) { return o.name == u.name; });
            const bool overridden = override_it != env.sky_uniforms.end();

            std::array<float, 4> temp{};
            for (uint32_t i = 0; i < count && i < u.default_value.size(); ++i) temp[i] = u.default_value[i];
            if (overridden) {
                for (uint32_t i = 0; i < count && i < override_it->count; ++i) temp[i] = override_it->values[i];
            }

            bool revert = false;
            if (override_row(u, std::span{temp}.first(count), overridden, "Use the shader's value", revert)) {
                WorldEnvironmentNode::SkyUniform value;
                value.name = u.name;
                value.count = count;
                value.values = temp;
                if (overridden) {
                    *override_it = std::move(value);
                } else {
                    env.sky_uniforms.push_back(std::move(value));
                }
                changed = true;
            }
            if (revert) revert_name = u.name;
        }
    }
    if (!revert_name.empty()) {
        std::erase_if(env.sky_uniforms,
                      [&](const WorldEnvironmentNode::SkyUniform& o) { return o.name == revert_name; });
        changed = true;
    }
    return changed;
}

// The fog material's uniforms as rows on a Volumetric Fog or Fog Volume node:
// each shows the node's own value when it has one and the material's
// otherwise, and an edit stores the node's value in `params`. A day profile or
// weather track of the same name folds over both while it is bound. Returns
// whether the node's values changed.
bool draw_fog_overrides(const Material* material, std::string_view default_shader,
                               std::vector<NamedValue>& params,
                               const std::vector<NamedValue>& material_values,
                               const InspectorContext& ictx) {
    bool changed = false;
    if (ictx.pipeline_cache == nullptr) return false;
    const uint32_t pid = material != nullptr
        ? material->pipeline_id()
        : ictx.pipeline_cache->register_shader(std::string(default_shader));
    if (pid >= ictx.pipeline_cache->shader_count()
        || ictx.pipeline_cache->has_compile_error(pid)) return false;
    const auto& meta = ictx.pipeline_cache->metadata(pid);
    if (meta.shader_type != ShaderType::Fog) return false;

    bool any = false;
    for (const auto& u : meta.uniforms) any |= u.type != UniformType::Sampler2D;
    if (!any) return false;

    auto box = ui::InsetGroup("##node_values", "This node", ui::icon::volumetric_fog,
                              theme::category(theme::Category::Environment));
    if (!box) return false;
    ui::hint("The material's values for this node alone; the material and other nodes sharing it keep their own");
    std::string revert_name;
    if (auto t = ui::PropertyTable("##node_values")) {
        for (const auto& u : meta.uniforms) {
            if (u.type == UniformType::Sampler2D) continue;
            const uint32_t count = uniform_float_count(u.type);
            const NamedValue* override = find_named(params, u.name);
            const NamedValue* base = find_named(material_values, u.name);

            std::array<float, 4> temp{};
            for (uint32_t i = 0; i < count && i < u.default_value.size(); ++i) temp[i] = u.default_value[i];
            if (base != nullptr) {
                for (uint32_t i = 0; i < count && i < base->count; ++i) temp[i] = base->values[i];
            }
            if (override != nullptr) {
                for (uint32_t i = 0; i < count && i < override->count; ++i) temp[i] = override->values[i];
            }

            bool revert = false;
            if (override_row(u, std::span{temp}.first(count), override != nullptr, "Use the material's value",
                             revert)) {
                NamedValue value;
                value.name = u.name;
                value.count = count;
                value.values = temp;
                value.srgb = u.hint == UniformHint::Color;
                set_named(params, std::move(value));
                changed = true;
            }
            if (revert) revert_name = u.name;
        }
    }
    if (!revert_name.empty()) {
        std::erase_if(params, [&](const NamedValue& v) { return v.name == revert_name; });
        changed = true;
    }
    return changed;
}

constexpr ui::Choice<CameraProjection> PROJECTIONS[] = {
    {CameraProjection::perspective, "Perspective"},
    {CameraProjection::orthographic, "Orthographic"},
};

constexpr ui::Choice<ColliderShape> VOLUME_SHAPES[] = {
    {ColliderShape::Box, "Box"},
    {ColliderShape::Sphere, "Sphere"},
};

// The name a moon phase goes by, on the slider rather than a number,
// because a quarter moon is the half-lit one and 0.5 reads as half.
const char* moon_phase_name(float phase) {
    return phase < 0.06F || phase > 0.94F ? "new"
         : phase < 0.19F ? "waxing crescent"
         : phase < 0.31F ? "first quarter (half)"
         : phase < 0.44F ? "waxing gibbous"
         : phase < 0.56F ? "full"
         : phase < 0.69F ? "waning gibbous"
         : phase < 0.81F ? "last quarter (half)"
                         : "waning crescent";
}

} // namespace

void PropertiesPanel::draw_transform(Node3D& node) {
    ui::section("Transform", ui::icon::transform);

    // Matches the viewport gizmo: Ctrl inverts the toggle.
    const bool snapping = grid_settings_ != nullptr
        && (grid_settings_->snap_enabled != ImGui::GetIO().KeyCtrl);
    if (!transform_editing_) {
        drag_start_transform_ = node.transform;
        drag_start_name_ = node.name;
    }

    ui::Edit edit;
    bool active = false;
    if (auto t = ui::PropertyTable("##transform")) {
        ui::row("Position", [&] {
            glm::vec3 position = node.transform.position;
            const ui::Edit e = ui::vec3("##position", position,
                                        {.speed = snapping ? grid_settings_->cell_size : 0.1f});
            if (e.changed) {
                node.transform.position = snapping
                    ? snap_to_grid(position, grid_settings_->cell_size) : position;
            }
            active = active || ImGui::IsItemActive();
            edit |= e;
        });
        ui::row("Rotation", [&] {
            glm::vec3 euler = node.transform.euler_degrees();
            const ui::Edit e = ui::vec3(
                "##rotation", euler,
                {.speed = snapping ? grid_settings_->rotation_snap_degrees : 1.0f,
                 .unit = ui::Unit::Degrees, .format = "%.1f"});
            if (e.changed) {
                node.transform.set_euler_degrees(
                    snapping ? snap_to_grid(euler, grid_settings_->rotation_snap_degrees) : euler);
            }
            active = active || ImGui::IsItemActive();
            edit |= e;
        });
        ui::row("Scale", [&] {
            glm::vec3 scale = node.transform.scale;
            const ui::Edit e = ui::vec3("##scale", scale,
                                        {.speed = snapping ? grid_settings_->scale_snap : 0.05f,
                                         .min = 0.01f, .max = 100.0f});
            if (e.changed) {
                // Rounding down to zero would collapse the object; hold
                // the same floor the drag field enforces.
                node.transform.scale = snapping
                    ? glm::max(snap_to_grid(scale, grid_settings_->scale_snap), glm::vec3(0.01f))
                    : scale;
            }
            active = active || ImGui::IsItemActive();
            edit |= e;
        });
        if (!node.source.empty()) {
            ui::row("Source", [&] {
                ImGui::AlignTextToFramePadding();
                ImGui::PushStyleColor(ImGuiCol_Text, theme::text_secondary());
                ImGui::TextUnformatted(node.source.c_str());
                ImGui::PopStyleColor();
                ImGui::SetItemTooltip("%s", node.source.c_str());
            });
        }
    }
    transform_editing_ = active;
    if (edit.changed) scene_->mark_dirty();
    if (edit.committed && history_ != nullptr) {
        history_->execute(std::make_unique<TransformCommand>(
            *scene_, node.id, drag_start_transform_, node.transform, node.name));
    }

    if (snapping) {
        const std::string label = std::string(ui::icon::snap) + "  Snap to grid";
        if (ui::button(label.c_str())) {
            const Transform before = node.transform;
            node.transform.position = snap_to_grid(node.transform.position, grid_settings_->cell_size);
            scene_->mark_dirty();
            if (history_ != nullptr) {
                history_->execute(std::make_unique<TransformCommand>(
                    *scene_, node.id, before, node.transform, node.name));
            }
        }
    }
}

void PropertiesPanel::draw_node_sections(Node3D& node, const InspectorContext& ictx) {
    // The section in one group, so the tracker knows whether a field in it
    // is being edited or under the mouse.
    ImGui::BeginGroup();
    bool changed = false;
    const char* title = nullptr;
    if (auto* camera = dynamic_cast<CameraNode*>(&node)) {
        title = "Camera";
        changed = draw_camera(*camera);
    } else if (auto* sun = dynamic_cast<DirectionalLightNode*>(&node)) {
        title = "Directional light";
        changed = draw_directional_light(*sun);
    } else if (auto* point = dynamic_cast<PointLightNode*>(&node)) {
        title = "Point light";
        changed = draw_point_light(*point);
    } else if (auto* spot = dynamic_cast<SpotLightNode*>(&node)) {
        title = "Spot light";
        changed = draw_spot_light(*spot);
    } else if (auto* fog = dynamic_cast<VolumetricFogNode*>(&node)) {
        title = "Volumetric fog";
        changed = draw_volumetric_fog(*fog, ictx);
    } else if (auto* volume = dynamic_cast<FogVolumeNode*>(&node)) {
        title = "Fog volume";
        changed = draw_fog_volume(*volume, ictx);
    } else if (auto* env = dynamic_cast<WorldEnvironmentNode*>(&node)) {
        title = "World environment";
        changed = draw_world_environment(*env, ictx);
    }
    ImGui::EndGroup();
    if (title == nullptr) return;

    // Fields here are a mix of the kit's, which say when an edit is done,
    // and older ones which only say they changed, so an edit counts as done
    // once nothing in the section is held and no picker or list is open.
    const bool active = ImGui::IsItemActive();
    const bool hovered = ImGui::IsItemHovered();
    const bool committed = !active && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
    node_edits_.track(
        *scene_, history_, node.id, 0, {changed, committed, active, hovered},
        [&] { return serialize_node_type(node); },
        [&](nlohmann::json before, nlohmann::json after) {
            return std::make_unique<NodeEditCommand>(*scene_, node.id, std::move(before),
                                                     std::move(after),
                                                     std::string("Edit ") + title);
        });
}

bool PropertiesPanel::draw_camera(CameraNode& camera) {
    bool changed = false;
    ui::section("Camera", ui::icon::camera);
    ui::Edit edit;
    if (auto t = ui::PropertyTable("##camera")) {
        ui::row("Projection", [&] { edit |= ui::choice("##projection", camera.projection, PROJECTIONS); });
        if (camera.projection == CameraProjection::perspective) {
            ui::row("Field of view", [&] {
                edit |= ui::drag("##fov", camera.fov, {.speed = 0.5f, .min = 1.0f, .max = 179.0f,
                                                       .unit = ui::Unit::Degrees, .format = "%.1f"});
            });
        } else {
            ui::row("Orthographic size", [&] {
                edit |= ui::drag("##ortho_size", camera.ortho_size,
                                 {.speed = 0.1f, .min = 0.1f, .max = 1000.0f, .unit = ui::Unit::Metres});
            });
        }
        ui::row("Near", [&] {
            // The lower bound is a depth-precision floor, not a taste call:
            // D32_SFLOAT spends its mantissa on the integer part as near
            // shrinks, and below ~0.01 the quantum at distance grows past a
            // typical AO/SSR sampling radius, banding every depth-derived
            // effect.
            edit |= ui::drag("##near", camera.near_plane,
                             {.speed = 0.01f, .min = 0.01f, .max = 100.0f, .unit = ui::Unit::Metres});
        });
        ui::row("Far", [&] {
            edit |= ui::drag("##far", camera.far_plane, {.speed = 10.0f, .min = 1.0f, .max = 100000.0f,
                                                         .unit = ui::Unit::Metres, .format = "%.0f"});
        });
    }
    return changed || edit.changed;
}

bool PropertiesPanel::draw_directional_light(DirectionalLightNode& light) {
    bool changed = false;
    ui::section("Directional light", ui::icon::directional_light);
    const auto* world = find_world_environment(*scene_);
    const bool sun_driven = world != nullptr && world->time_of_day_enabled;
    ui::Edit edit;
    if (auto t = ui::PropertyTable("##directional")) {
        ui::row("Direction", [&] {
            ImGui::BeginDisabled(sun_driven);
            edit |= ui::vec3("##direction", light.direction, {.speed = 0.01f, .min = -1.0f, .max = 1.0f});
            ImGui::EndDisabled();
            if (sun_driven) {
                ImGui::SetItemTooltip("Driven by the World Environment's time of day");
            }
        });
        ui::row("Colour", [&] { edit |= ui::color("##colour", light.color, ui::ColorSpace::Linear); });
        ui::row("Intensity", [&] {
            edit |= ui::drag("##intensity", light.intensity, {.speed = 0.05f, .min = 0.0f, .max = 10.0f});
        });
        ui::row("Ambient", [&] { edit |= ui::color("##ambient", light.ambient_color, ui::ColorSpace::Linear); });
        ui::row("Ambient intensity", [&] {
            edit |= ui::drag("##ambient_intensity", light.ambient_intensity,
                             {.speed = 0.05f, .min = 0.0f, .max = 2.0f});
        });
    }
    return changed || edit.changed;
}

bool PropertiesPanel::draw_point_light(PointLightNode& light) {
    bool changed = false;
    ui::section("Point light", ui::icon::point_light);
    ui::Edit edit;
    if (auto t = ui::PropertyTable("##point")) {
        ui::row("Colour", [&] { edit |= ui::color("##colour", light.color, ui::ColorSpace::Linear); });
        ui::row("Intensity", [&] {
            edit |= ui::drag("##intensity", light.intensity, {.speed = 0.1f, .min = 0.0f, .max = 100.0f});
        });
        ui::row("Radius", [&] {
            edit |= ui::drag("##radius", light.radius,
                             {.speed = 0.5f, .min = 0.1f, .max = 500.0f, .unit = ui::Unit::Metres});
        });
        ui::row("Volumetric scatter", [&] {
            edit |= ui::drag("##scatter", light.vol_scatter_intensity,
                             {.speed = 0.05f, .min = 0.0f, .max = 10.0f});
        });
        ui::row("Cast shadows", [&] { edit |= ui::checkbox("##shadows", light.cast_shadows); });
        if (light.cast_shadows) {
            ui::row("Shadow bias", [&] {
                edit |= ui::drag("##bias", light.shadow_bias,
                                 {.speed = 0.0005f, .min = 0.0f, .max = 0.05f, .format = "%.4f"});
            });
        }
    }
    return changed || edit.changed;
}

bool PropertiesPanel::draw_spot_light(SpotLightNode& light) {
    bool changed = false;
    ui::section("Spot light", ui::icon::spot_light);
    ui::Edit edit;
    if (auto t = ui::PropertyTable("##spot")) {
        ui::row("Direction", [&] {
            edit |= ui::vec3("##direction", light.direction, {.speed = 0.01f, .min = -1.0f, .max = 1.0f});
        });
        ui::row("Colour", [&] { edit |= ui::color("##colour", light.color, ui::ColorSpace::Linear); });
        ui::row("Intensity", [&] {
            edit |= ui::drag("##intensity", light.intensity, {.speed = 0.1f, .min = 0.0f, .max = 100.0f});
        });
        ui::row("Radius", [&] {
            edit |= ui::drag("##radius", light.radius,
                             {.speed = 0.5f, .min = 0.1f, .max = 500.0f, .unit = ui::Unit::Metres});
        });
        ui::row("Inner angle", [&] {
            edit |= ui::drag("##inner", light.inner_angle, {.speed = 0.5f, .min = 0.0f, .max = 90.0f,
                                                            .unit = ui::Unit::Degrees, .format = "%.1f"});
        });
        ui::row("Outer angle", [&] {
            edit |= ui::drag("##outer", light.outer_angle, {.speed = 0.5f, .min = 0.0f, .max = 90.0f,
                                                            .unit = ui::Unit::Degrees, .format = "%.1f"});
        });
        ui::row("Volumetric scatter", [&] {
            edit |= ui::drag("##scatter", light.vol_scatter_intensity,
                             {.speed = 0.05f, .min = 0.0f, .max = 10.0f});
        });
        ui::row("Cast shadows", [&] { edit |= ui::checkbox("##shadows", light.cast_shadows); });
        if (light.cast_shadows) {
            ui::row("Shadow bias", [&] {
                edit |= ui::drag("##bias", light.shadow_bias,
                                 {.speed = 0.0005f, .min = 0.0f, .max = 0.05f, .format = "%.4f"});
            });
        }
    }
    return changed || edit.changed;
}

bool PropertiesPanel::draw_volumetric_fog(VolumetricFogNode& fog, const InspectorContext& ictx) {
    bool changed = false;
    ui::section("Volumetric fog", ui::icon::volumetric_fog);
    // How the haze looks is its material; the node keeps where it is, how
    // far it reaches and how it answers movement.
    static const std::string fog_mat_exts[] = {".fjmat"};
    if (auto t = ui::PropertyTable("##fog_material")) {
        ui::row("Material", [&] {
            changed |= asset_slot({.label = "Material",
                                   .id = "##material",
                                   .extensions = fog_mat_exts,
                                   .ctx = &ictx,
                                   .shader_types = only_shader_type(ShaderType::Fog)},
                                  fog.material_path);
        });
    }
    if (fog.material != nullptr) {
        draw_material_inspector_for(fog.material);
    } else {
        const std::string text = "Drawn with the engine's " + std::string(DEFAULT_FOG_SHADER);
        ui::hint(text.c_str());
    }
    changed |= draw_fog_overrides(fog.material, DEFAULT_FOG_SHADER, fog.params, fog.material_values_, ictx);

    ui::Edit edit;
    if (auto t = ui::PropertyTable("##fog")) {
        ui::row("Max distance", [&] {
            edit |= ui::slider("##distance", fog.max_distance, 10.0f, 1000.0f, ui::Unit::Metres, "%.0f");
        });
        ui::row("Temporal blend", [&] {
            edit |= ui::slider("##temporal", fog.temporal_blend, 0.0f, 0.99f);
        });
    }

    ui::subheading("Sun shafts");
    if (auto t = ui::PropertyTable("##shafts")) {
        ui::row("Enabled", [&] { edit |= ui::checkbox("##enabled", fog.shafts_enabled); });
        if (fog.shafts_enabled) {
            ui::row("Intensity", [&] { edit |= ui::slider("##intensity", fog.shaft_intensity, 0.0f, 10.0f); });
            ui::row("Density multiplier", [&] {
                edit |= ui::slider("##density", fog.shaft_density_mult, 0.0f, 5.0f);
            });
            ui::row("Max reach", [&] {
                edit |= ui::slider("##reach", fog.shaft_max_reach, 10.0f, 1000.0f, ui::Unit::Metres, "%.0f");
            });
            ui::row("Steps", [&] { edit |= ui::slider_int("##steps", fog.shaft_step_count, 8, 128); });
            ui::row("Anisotropy", [&] {
                edit |= ui::slider("##anisotropy", fog.shaft_anisotropy, 0.0f, 0.9f);
            });
            ui::row("Directionality", [&] {
                edit |= ui::slider("##directionality", fog.shaft_directionality, 0.0f, 1.0f);
            });
            ui::row("Threshold", [&] { edit |= ui::slider("##threshold", fog.shaft_threshold, 0.0f, 0.95f); });
        }
    }

    ui::subheading("Motion");
    if (auto t = ui::PropertyTable("##fog_motion")) {
        ui::row("Response", [&] { edit |= ui::slider("##response", fog.motion_response, 0.0f, 1.0f); });
        if (fog.motion_response <= 0.0f) {
            ui::hint("Ignores things moving through it");
        } else {
            ui::hint("Faint in open haze: a few metres of a long column. Fog volumes carry a wake");
        }
        ui::row("Show field", [&] {
            bool on = fog.density_field_debug > 0.0f;
            const ui::Edit e = ui::checkbox("##field", on);
            if (e.changed) fog.density_field_debug = on ? 1.0f : 0.0f;
            edit |= e;
        });
        if (fog.density_field_debug > 0.0f) {
            ui::hint("Red is air pushed aside, blue piled up");
        }
    }
    return changed || edit.changed;
}

bool PropertiesPanel::draw_fog_volume(FogVolumeNode& volume, const InspectorContext& ictx) {
    bool changed = false;
    ui::section("Fog volume", ui::icon::fog_volume);
    // What the bank looks like is its material; the node keeps the shape,
    // how the bank drifts and how it answers movement.
    static const std::string volume_mat_exts[] = {".fjmat"};
    if (auto t = ui::PropertyTable("##volume_material")) {
        ui::row("Material", [&] {
            changed |= asset_slot({.label = "Material",
                                   .id = "##material",
                                   .extensions = volume_mat_exts,
                                   .ctx = &ictx,
                                   .shader_types = only_shader_type(ShaderType::Fog)},
                                  volume.material_path);
        });
    }
    if (volume.material != nullptr) {
        draw_material_inspector_for(volume.material);
    } else {
        const std::string text = "Drawn with the engine's " + std::string(DEFAULT_FOG_VOLUME_SHADER);
        ui::hint(text.c_str());
    }
    changed |= draw_fog_overrides(volume.material, DEFAULT_FOG_VOLUME_SHADER, volume.params,
                       volume.material_values_, ictx);

    ui::Edit edit;
    if (auto t = ui::PropertyTable("##fog_volume")) {
        ui::row("Shape", [&] { edit |= ui::choice("##shape", volume.shape, VOLUME_SHAPES); });
        ui::hint("Size comes from the node's scale");
        ui::row("Edge falloff", [&] { edit |= ui::slider("##falloff", volume.edge_falloff, 0.0f, 1.0f); });
        ui::row("Group", [&] { edit |= ui::text_field("##group", volume.fog_group); });
        if (!volume.fog_group.empty()) {
            const std::string text = "Driven by fog." + volume.fog_group + ".<uniform> tracks";
            ui::hint(text.c_str());
        }
    }

    // How the bank's field moves through the world. What the field is, the
    // wisps' size and contrast, is the material's.
    ui::subheading("Drift");
    if (auto t = ui::PropertyTable("##drift")) {
        ui::row("Direction", [&] {
            edit |= ui::vec3("##direction", volume.drift_direction, {.speed = 0.01f, .min = -1.0f, .max = 1.0f});
        });
        ui::row("Speed", [&] {
            edit |= ui::slider("##speed", volume.drift_speed, 0.0f, 5.0f, ui::Unit::MetresPerSecond);
        });
        ui::row("Wind", [&] { edit |= ui::slider("##wind", volume.wind_influence, 0.0f, 1.0f); });
        if (volume.wind_influence > 0.0f) {
            ui::row("Wind scale", [&] {
                edit |= ui::slider("##wind_scale", volume.wind_speed_scale, 0.0f, 1.0f);
            });
        }
    }

    ui::subheading("Motion");
    if (auto t = ui::PropertyTable("##volume_motion")) {
        ui::row("Response", [&] { edit |= ui::slider("##response", volume.motion_response, 0.0f, 1.0f); });
        if (volume.motion_response <= 0.0f) {
            ui::hint("Ignores things moving through it");
        }
        ui::row("Vortices", [&] { edit |= ui::slider("##vortices", volume.vortex_strength, 0.0f, 2.0f); });
        if (volume.motion_response > 0.0f && volume.vortex_strength > 0.0f) {
            ui::hint("Rolls shed off each flank of a wake");
        }
        ui::row("Compression", [&] { edit |= ui::slider("##compression", volume.compression, 0.0f, 3.0f); });
        ui::row("Show field", [&] {
            bool on = volume.density_field_debug > 0.0f;
            const ui::Edit e = ui::checkbox("##field", on);
            if (e.changed) volume.density_field_debug = on ? 1.0f : 0.0f;
            edit |= e;
        });
        if (volume.density_field_debug > 0.0f) {
            ui::hint("Red is air pushed aside, blue piled up");
        }
    }
    return changed || edit.changed;
}

bool PropertiesPanel::draw_world_environment(WorldEnvironmentNode& env, const InspectorContext& ictx) {
    bool changed = false;
    ui::section("World environment", ui::icon::world_environment);
    static const std::string hdri_exts[] = {".hdr", ".exr"};
    static const std::string sky_shader_exts[] = {".fjsl"};
    static const std::string day_profile_exts[] = {".fjday"};
    static const std::string weather_exts[] = {".fjweather"};
    static const std::string climate_exts[] = {".fjclimate"};
    if (auto t = ui::PropertyTable("##sky_assets")) {
        ui::row("HDRI", [&] {
            changed |= asset_slot({.label = "HDRI", .id = "##hdri", .extensions = hdri_exts, .ctx = &ictx},
                                  env.hdri_path);
        });
        ui::row("Sky shader", [&] {
            changed |= asset_slot({.label = "Sky shader",
                                   .id = "##sky_shader",
                                   .extensions = sky_shader_exts,
                                   .ctx = &ictx,
                                   .shader_types = only_shader_type(ShaderType::Sky)},
                                  env.sky_shader);
        });
    }
    if (!env.sky_shader.empty()) {
        changed |= draw_sky_uniforms(env, ictx);
    }
    if (auto t = ui::PropertyTable("##time_assets")) {
        ui::row("Day profile", [&] {
            changed |= asset_slot(
                {.label = "Day profile", .id = "##day_profile", .extensions = day_profile_exts, .ctx = &ictx},
                env.day_profile);
        });
        ui::row("Weather", [&] {
            changed |= asset_slot(
                {.label = "Weather", .id = "##weather", .extensions = weather_exts, .ctx = &ictx},
                env.weather);
        });
        ui::row("Climate", [&] {
            changed |= asset_slot(
                {.label = "Climate", .id = "##climate", .extensions = climate_exts, .ctx = &ictx},
                env.climate);
        });
    }
    changed |= draw_weather_controls(env, ictx);

    ui::Edit edit;
    const bool gradient = env.hdri_path.empty();
    if (auto t = ui::PropertyTable("##sky")) {
        ImGui::BeginDisabled(!gradient);
        ui::row("Sky top", [&] { edit |= ui::color("##top", env.sky_top_color, ui::ColorSpace::Linear); });
        ui::row("Sky bottom", [&] {
            edit |= ui::color("##bottom", env.sky_bottom_color, ui::ColorSpace::Linear);
        });
        ImGui::EndDisabled();
        if (!gradient) {
            ui::hint("The HDRI replaces the gradient");
        }
        ui::row("IBL intensity", [&] { edit |= ui::slider("##ibl", env.ibl_intensity, 0.0f, 5.0f); });
    }

    ui::subheading("Contact shadows");
    if (auto t = ui::PropertyTable("##contact")) {
        ui::row("Enabled", [&] { edit |= ui::checkbox("##enabled", env.contact_shadows_enabled); });
        if (env.contact_shadows_enabled) {
            ui::row("Distance", [&] {
                edit |= ui::slider("##distance", env.contact_shadow_distance, 0.05f, 2.0f, ui::Unit::Metres);
            });
            ui::row("Thickness", [&] {
                edit |= ui::slider("##thickness", env.contact_shadow_thickness, 0.01f, 0.5f, ui::Unit::Metres);
            });
            ui::row("Steps", [&] { edit |= ui::slider_int("##steps", env.contact_shadow_steps, 4, 64); });
        }
    }

    ui::subheading("Time of day");
    if (auto t = ui::PropertyTable("##time_of_day")) {
        ui::row("Enabled", [&] { edit |= ui::checkbox("##enabled", env.time_of_day_enabled); });
        if (env.time_of_day_enabled) {
            ui::row("Hour", [&] {
                const int minutes = static_cast<int>(env.time_of_day * 60.0f + 0.5f) % (24 * 60);
                char clock[8];
                std::snprintf(clock, sizeof(clock), "%02d:%02d", minutes / 60, minutes % 60);
                edit |= ui::slider_labelled("##hour", env.time_of_day, 0.0f, 24.0f, clock);
            });
            ui::row("Day length", [&] {
                float minutes = env.day_length / 60.0f;
                const ui::Edit e = ui::drag("##day_length", minutes,
                                            {.speed = 0.5f, .min = 0.0f, .max = 24.0f * 60.0f,
                                             .unit = ui::Unit::Minutes, .format = "%.1f"});
                if (e.changed) env.day_length = std::max(minutes, 0.0f) * 60.0f;
                edit |= e;
            });
            ui::row("Run in editor", [&] { edit |= ui::checkbox("##run", env.advance_in_editor); });
            ui::row("Path tilt", [&] {
                edit |= ui::slider("##tilt", env.sun_path_tilt, 0.0f, 90.0f, ui::Unit::Degrees, "%.0f");
            });
            ui::row("North", [&] {
                edit |= ui::slider("##north", env.north_yaw, -180.0f, 180.0f, ui::Unit::Degrees, "%.0f");
            });
            ui::row("Noon hour", [&] { edit |= ui::slider("##noon", env.noon_hour, 0.0f, 24.0f, ui::Unit::None, "%.1f"); });
        }
    }
    if (env.time_of_day_enabled) {
        ui::subheading("Moon");
        if (auto t = ui::PropertyTable("##moon")) {
            ui::row("Phase",
                    "Where the moon rides the sun's path, and so how much of it is lit: new at 0 "
                    "and 1, half at 0.25 and 0.75, full at 0.5.",
                    [&] {
                        edit |= ui::slider_labelled("##phase", env.moon_phase, 0.0f, 1.0f,
                                                    moon_phase_name(env.moon_phase));
                    });
            ui::row("Colour", [&] { edit |= ui::color("##colour", env.moon_color, ui::ColorSpace::Linear); });
            ui::row("Intensity", [&] {
                edit |= ui::drag("##intensity", env.moon_intensity,
                                 {.speed = 0.005f, .min = 0.0f, .max = 2.0f, .format = "%.3f"});
            });
        }
    }

    ui::subheading("Atmospheric scattering");
    if (auto t = ui::PropertyTable("##atmosphere")) {
        ui::row("Enabled", [&] { edit |= ui::checkbox("##enabled", env.atmosphere_enabled); });
        if (env.atmosphere_enabled) {
            ui::row("Sun intensity", [&] {
                edit |= ui::slider("##sun", env.atmosphere_sun_intensity, 1.0f, 50.0f);
            });
            ui::row("Rayleigh height", [&] {
                edit |= ui::slider("##rayleigh", env.atmosphere_rayleigh_scale, 1000.0f, 20000.0f,
                                   ui::Unit::Metres, "%.0f");
            });
            ui::row("Mie height", [&] {
                edit |= ui::slider("##mie_height", env.atmosphere_mie_scale, 100.0f, 5000.0f,
                                   ui::Unit::Metres, "%.0f");
            });
            ui::row("Mie anisotropy", [&] { edit |= ui::slider("##mie_g", env.atmosphere_mie_g, 0.0f, 0.99f); });
            ui::row("Mie coefficient", [&] {
                edit |= ui::slider("##mie_coeff", env.atmosphere_mie_coeff, 1e-6f, 100e-6f,
                                   ui::Unit::None, "%.1e");
            });
            ui::row("HDRI floor", [&] {
                edit |= ui::slider("##hdri_floor", env.atmosphere_hdri_floor, 0.0f, 1.0f);
            });
        }
    }
    return changed || edit.changed;
}

// Below the Weather reference: a pick list of the project's events, and
// what is playing right now with Stop and Replay. Setting the field starts
// the event; the pick list is the same assignment without a file dialog.
bool PropertiesPanel::draw_weather_controls(WorldEnvironmentNode& env, const InspectorContext& ictx) {
    bool changed = false;
    const Project* project = ictx.project_mgr != nullptr ? ictx.project_mgr->active() : nullptr;
    const double now = ImGui::GetTime();
    if (weather_files_scanned_at_ <= 0.0 || now - weather_files_scanned_at_ > 2.0) {
        weather_files_scanned_at_ = now;
        weather_files_ = list_project_assets(project, ".fjweather");
    }

    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##EnvWeatherPick", "Pick an event...")) {
        for (const auto& file : weather_files_) {
            const std::string name = std::filesystem::path(file).stem().string();
            if (ImGui::Selectable(name.c_str(), file == env.weather)) {
                env.weather = file;
                changed = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", file.c_str());
        }
        if (weather_files_.empty()) ImGui::TextDisabled("No .fjweather files in the project");
        ImGui::EndCombo();
    }

    // Which phase the scene opens in, saved with it. Unlike the Replay
    // control below, this is not an action: it decides what the first frame
    // of the scene looks like, in the editor and on launch alike.
    if (!env.weather.empty()) {
        if (auto t = PropertyTable("##weather_start")) {
            prop_row("Start Phase", [&] {
                if (ImGui::BeginCombo("##StartPhase",
                                      weather_phase_name(env.weather_start_phase))) {
                    for (uint32_t i = 0; i < WEATHER_PHASE_COUNT; ++i) {
                        const auto phase = static_cast<WeatherPhase>(i);
                        if (ImGui::Selectable(weather_phase_name(phase),
                                              phase == env.weather_start_phase)) {
                            env.weather_start_phase = phase;
                            changed = true;
                            // Show it at once rather than on the next scene load.
                            if (environment_ != nullptr && ictx.resource_manager != nullptr) {
                                if (auto handle = ictx.resource_manager->load_weather_asset(env.weather)) {
                                    environment_->play_weather(*handle, phase);
                                }
                            }
                        }
                    }
                    ImGui::EndCombo();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Where the event is when the scene opens. Approach is "
                                      "weather arriving; a later phase is weather that set in "
                                      "before the scene did.");
                }
            });
        }
    }

    if (environment_ == nullptr || ictx.resource_manager == nullptr) return changed;
    const auto& weather = environment_->state().weather;
    if (weather.event_id != 0) {
        const auto* event = ictx.resource_manager->get(WeatherAssetHandle{weather.event_id});
        ImGui::TextDisabled("Playing %s: %s %d%%", event != nullptr ? event->name.c_str() : "?",
                            weather_phase_name(weather.phase),
                            static_cast<int>(weather.phase_progress * 100.0f + 0.5f));
        if (ImGui::SmallButton("Stop##EnvWeatherStop")) {
            environment_->play_weather({});
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Let the event tail out with nothing to follow");
        ImGui::SameLine();
    } else {
        ImGui::TextDisabled("No weather playing");
    }
    // With a climate running, what it will play next.
    if (!env.climate.empty()) {
        std::string line;
        for (uint32_t i = 0; i < 2; ++i) {
            const std::string path = environment_->forecast_event(i);
            if (path.empty()) break;
            if (!line.empty()) line += ", ";
            line += std::filesystem::path(path).stem().string();
        }
        if (!line.empty()) ImGui::TextDisabled("Next: %s", line.c_str());
        bool held = environment_->weather_held();
        if (ImGui::Checkbox("Hold##EnvWeatherHold", &held)) environment_->hold_weather(held);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Freeze the climate: what plays keeps playing and nothing new is rolled");
        }
        ImGui::SameLine();
    }
    if (!env.weather.empty()) {
        auto replay = [&](WeatherPhase phase) {
            if (auto handle = ictx.resource_manager->load_weather_asset(env.weather)) {
                environment_->play_weather(*handle, phase);
            }
        };
        // The button replays from the start; the arrow plays from a phase,
        // on the pick rather than on a second press.
        if (ImGui::SmallButton("Replay##EnvWeatherReplay")) replay(WeatherPhase::Approach);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Start the scene's weather event again");
        ImGui::SameLine(0.0f, 2.0f);
        if (ImGui::SmallButton("v##EnvWeatherFrom")) ImGui::OpenPopup("##EnvWeatherFromMenu");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Replay from a later phase");
        if (ImGui::BeginPopup("##EnvWeatherFromMenu")) {
            for (uint32_t i = 0; i < WEATHER_PHASE_COUNT; ++i) {
                const auto phase = static_cast<WeatherPhase>(i);
                std::string label = std::string("Replay from ") + weather_phase_name(phase);
                if (ImGui::MenuItem(label.c_str())) replay(phase);
            }
            ImGui::EndPopup();
        }
    }
    return changed;
}

} // namespace fjell
