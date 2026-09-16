#include "ui/properties_panel.hpp"
#include "core/base_path.hpp"
#include "core/command_history.hpp"
#include "core/commands.hpp"
#include "core/log.hpp"
#include "core/project_manager.hpp"
#include "renderer/bindless.hpp"
#include "renderer/resources/fjsl_parser.hpp"
#include "renderer/resources/material.hpp"
#include "renderer/components/mesh_component.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/components/skeletal_mesh_component.hpp"
#include "renderer/components/terrain_component.hpp"
#include "renderer/uniform_types.hpp"
#include "scene/camera_node.hpp"
#include "scene/component_registry.hpp"
#include "scene/light_node.hpp"
#include "environment/environment_resolver.hpp"
#include "scene/scene.hpp"
#include "renderer/pass/grid_pass.hpp"
#include "scripting/script_manager.hpp"
#include "ui/icon_cache.hpp"
#include "ui/icons_lc.hpp"
#include "ui/asset_ref_widget.hpp"
#include "ui/inspector_widgets.hpp"
#include "renderer/pass/sky_pass.hpp"
#include "core/string_utils.hpp"
#include "ui/material_params_widget.hpp"
#include "ui/panel_widget.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <span>
#include <cstdio>
#include <glm/gtc/type_ptr.hpp>

#include <cmath>
#include <cstring>

namespace fjell {

static glm::vec3 snap_to_grid(const glm::vec3& pos, float cell_size) {
    return {
        std::round(pos.x / cell_size) * cell_size,
        std::round(pos.y / cell_size) * cell_size,
        std::round(pos.z / cell_size) * cell_size,
    };
}

// The sky shader's uniforms as rows on the World Environment node: each shows
// the node's override when it has one and the .fjsl default otherwise, and an
// edit stores an override on the node. A day profile track of the same name
// takes over from the override while the profile is bound.
static void draw_sky_uniforms(WorldEnvironmentNode& env, const InspectorContext& ictx) {
    if (ictx.pipeline_cache == nullptr) return;
    const Project* project = ictx.project_mgr != nullptr ? ictx.project_mgr->active() : nullptr;
    const uint32_t pid = ictx.pipeline_cache->register_shader(
        canonical_sky_shader_path(env.sky_shader, project));
    if (ictx.pipeline_cache->has_compile_error(pid)) return;
    const auto& meta = ictx.pipeline_cache->metadata(pid);
    if (meta.shader_type != "sky") return;

    bool any = false;
    for (const auto& u : meta.uniforms) any |= u.type != UniformType::Sampler2D;
    if (!any) return;

    ImGui::TextDisabled("Sky Uniforms");
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

        ImGui::PushID(u.name.c_str());
        ImGui::Text("%s", display_name(u.name).c_str());
        if (overridden) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset")) {
                env.sky_uniforms.erase(override_it);
                if (ictx.scene != nullptr) ictx.scene->mark_dirty();
                ImGui::PopID();
                continue;
            }
        }
        ImGui::SetNextItemWidth(-1);
        if (draw_uniform_value(u, std::span{temp}.first(count))) {
            WorldEnvironmentNode::SkyUniform value;
            value.name = u.name;
            value.count = count;
            value.values = temp;
            if (overridden) {
                *override_it = std::move(value);
            } else {
                env.sky_uniforms.push_back(std::move(value));
            }
            if (ictx.scene != nullptr) ictx.scene->mark_dirty();
        }
        ImGui::PopID();
    }
}

void PropertiesPanel::init(Scene* scene, uint32_t* selected_id,
                           ScriptManager* scripts,
                           CommandHistory* history, GridSettings* grid_settings,
                           PipelineCache* pipeline_cache,
                           BindlessTextures* bindless,
                           ResourceManager* resource_manager,
                           ProjectManager* project_mgr,
                           ContentBrowser* content_browser,
                           FileBrowser* file_browser,
                           IconCache* icons, TerrainSculptTool* sculpt) {
    scene_ = scene;
    selected_id_ = selected_id;
    scripts_ = scripts;
    history_ = history;
    grid_settings_ = grid_settings;
    pipeline_cache_ = pipeline_cache;
    bindless_ = bindless;
    resource_manager_ = resource_manager;
    project_mgr_ = project_mgr;
    content_browser_ = content_browser;
    file_browser_ = file_browser;
    icons_ = icons;
    sculpt_ = sculpt;
}

void PropertiesPanel::draw() {
    if (auto p = Panel(ICON_LC_SLIDERS_HORIZONTAL, "Properties")) {
        auto* base = scene_->find(*selected_id_);
        auto* obj = base ? base->as_spatial() : nullptr;
        if (base) {

            // Editable name
            char name_buf[128];
            std::strncpy(name_buf, base->name.c_str(), sizeof(name_buf) - 1);
            name_buf[sizeof(name_buf) - 1] = '\0';
            std::string old_name = base->name;
            if (auto t = PropertyTable("##name")) {
                prop_row("Name", [&] {
                    if (ImGui::InputText("##Name", name_buf, sizeof(name_buf))) {
                        base->name = name_buf;
                        scene_->mark_dirty();
                    }
                });
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && history_ && base->name != old_name) {
                history_->execute(std::make_unique<RenameCommand>(
                    *scene_, base->id, old_name, base->name));
            }

            if (obj) {
            section_header("TRANSFORM", ICON_LC_MOVE_3D);
            // Matches the viewport gizmo: Ctrl inverts the toggle.
            const bool snapping = grid_settings_
                && (grid_settings_->snap_enabled != ImGui::GetIO().KeyCtrl);
            float drag_speed = snapping ? grid_settings_->cell_size : 0.1f;
            if (auto t = PropertyTable("##xform")) {
                prop_row("Position", [&] {
                    if (ImGui::DragFloat3("##Position", &obj->transform.position.x, drag_speed)) {
                        if (snapping) {
                            obj->transform.position = snap_to_grid(obj->transform.position, grid_settings_->cell_size);
                        }
                        scene_->mark_dirty();
                    }
                    if (ImGui::IsItemActivated()) {
                        drag_start_transform_ = obj->transform;
                        drag_start_name_ = obj->name;
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit() && history_) {
                        history_->execute(std::make_unique<TransformCommand>(
                            *scene_, obj->id, drag_start_transform_, obj->transform, obj->name));
                    }
                });
                prop_row("Rotation", [&] {
                    glm::vec3 euler = obj->transform.euler_degrees();
                    float rot_speed = snapping ? grid_settings_->rotation_snap_degrees : 1.0f;
                    if (ImGui::DragFloat3("##Rotation", &euler.x, rot_speed)) {
                        if (snapping) {
                            euler = snap_to_grid(euler, grid_settings_->rotation_snap_degrees);
                        }
                        obj->transform.set_euler_degrees(euler);
                        scene_->mark_dirty();
                    }
                    if (ImGui::IsItemActivated()) {
                        drag_start_transform_ = obj->transform;
                        drag_start_name_ = obj->name;
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit() && history_) {
                        history_->execute(std::make_unique<TransformCommand>(
                            *scene_, obj->id, drag_start_transform_, obj->transform, obj->name));
                    }
                });
                prop_row("Scale", [&] {
                    float scale_speed = snapping ? grid_settings_->scale_snap : 0.05f;
                    if (ImGui::DragFloat3("##Scale", &obj->transform.scale.x, scale_speed, 0.01f, 100.0f)) {
                        if (snapping) {
                            // Rounding down to zero would collapse the object;
                            // hold the same floor the drag field enforces.
                            glm::vec3 snapped = snap_to_grid(obj->transform.scale,
                                                             grid_settings_->scale_snap);
                            obj->transform.scale = glm::max(snapped, glm::vec3(0.01f));
                        }
                        scene_->mark_dirty();
                    }
                    if (ImGui::IsItemActivated()) {
                        drag_start_transform_ = obj->transform;
                        drag_start_name_ = obj->name;
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit() && history_) {
                        history_->execute(std::make_unique<TransformCommand>(
                            *scene_, obj->id, drag_start_transform_, obj->transform, obj->name));
                    }
                });
            }
            if (snapping) {
                if (ImGui::SmallButton(ICON_LC_MAGNET " Snap to grid")) {
                    Transform before = obj->transform;
                    obj->transform.position = snap_to_grid(obj->transform.position, grid_settings_->cell_size);
                    scene_->mark_dirty();
                    if (history_) {
                        history_->execute(std::make_unique<TransformCommand>(
                            *scene_, obj->id, before, obj->transform, obj->name));
                    }
                }
            }

            if (!obj->source.empty()) {
                ImGui::Spacing();
                ImGui::TextDisabled(ICON_LC_LINK "  Source: %s", obj->source.c_str());
            }

            // ── Components ────────────────────────────────────────────
            section_header("COMPONENTS", ICON_LC_PACKAGE);

            const InspectorContext ictx{
                scene_, history_, resource_manager_, pipeline_cache_, bindless_, project_mgr_,
                content_browser_, file_browser_, icons_, sculpt_};

            auto& registry = ComponentRegistry::instance();
            ComponentTypeId remove_id = ~0u;

            obj->components.for_each([&](ComponentTypeId id, void* ptr) {
                const auto* meta = registry.find(id);
                if (!meta) return;

                colored_separator(meta->color);
                if (ImGui::CollapsingHeader(meta->display_name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                    meta->draw(*obj, ptr, ictx);

                    // Material inspector for mesh components
                    if (id == component_type_id<MeshComponent>())
                        draw_material_inspector(*obj);
                    else if (id == component_type_id<SkeletalMeshComponent>())
                        draw_material_inspector_for(static_cast<SkeletalMeshComponent*>(ptr)->material);
                    else if (id == component_type_id<TerrainComponent>())
                        draw_material_inspector_for(static_cast<TerrainComponent*>(ptr)->material);

                    std::string remove_label = "Remove " + meta->display_name;
                    if (remove_button(remove_label.c_str())) {
                        remove_id = id;
                    }
                }
            });

            if (remove_id != ~0u) {
                obj->components.remove_by_id(remove_id);
                scene_->mark_dirty();
            }

            // ── Node subclass inspectors ─────────────────────────────

            // Camera properties
            if (auto* cam = dynamic_cast<CameraNode*>(obj)) {
                section_header("CAMERA", ICON_LC_CAMERA);
                if (auto t = PropertyTable("##cam")) {
                    prop_row("Projection", [&] {
                        int proj_mode = static_cast<int>(cam->projection);
                        const char* proj_modes[] = {"Perspective", "Orthographic"};
                        if (ImGui::Combo("##CamProjection", &proj_mode, proj_modes, 2)) {
                            cam->projection = static_cast<CameraProjection>(proj_mode);
                            scene_->mark_dirty();
                        }
                    });
                    if (cam->projection == CameraProjection::perspective) {
                        prop_row("FOV", [&] {
                            if (ImGui::DragFloat("##CamFOV", &cam->fov, 0.5f, 1.0f, 179.0f)) {
                                scene_->mark_dirty();
                            }
                        });
                    } else {
                        prop_row("Ortho Size", [&] {
                            if (ImGui::DragFloat("##CamOrthoSize", &cam->ortho_size, 0.1f, 0.1f, 1000.0f)) {
                                scene_->mark_dirty();
                            }
                        });
                    }
                    prop_row("Near", [&] {
                        // Lower bound is a depth-precision floor, not a taste
                        // call: D32_SFLOAT spends its mantissa on the integer
                        // part as near shrinks, and below ~0.01 the quantum at
                        // distance grows past a typical AO/SSR sampling radius,
                        // banding every depth-derived effect.
                        if (ImGui::DragFloat("##CamNear", &cam->near_plane, 0.01f, 0.01f, 100.0f)) {
                            scene_->mark_dirty();
                        }
                    });
                    prop_row("Far", [&] {
                        if (ImGui::DragFloat("##CamFar", &cam->far_plane, 10.0f, 1.0f, 100000.0f)) {
                            scene_->mark_dirty();
                        }
                    });
                }
            }

            // Light properties
            if (auto* dl = dynamic_cast<DirectionalLightNode*>(obj)) {
                section_header("DIRECTIONAL LIGHT", ICON_LC_SUN);
                const auto* world = scene_ != nullptr ? find_world_environment(*scene_) : nullptr;
                const bool sun_driven = world != nullptr && world->time_of_day_enabled;
                if (auto t = PropertyTable("##dirlight")) {
                    prop_row("Direction", [&] {
                        ImGui::BeginDisabled(sun_driven);
                        ImGui::DragFloat3("##LightDir", &dl->direction.x, 0.01f, -1.0f, 1.0f);
                        ImGui::EndDisabled();
                        if (sun_driven && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                            ImGui::SetTooltip("Driven by the World Environment's time of day");
                        }
                    });
                    prop_row("Color", [&] {
                        color_edit_linear("##LightColor", dl->color);
                    });
                    prop_row("Intensity", [&] {
                        ImGui::DragFloat("##LightIntensity", &dl->intensity, 0.05f, 0.0f, 10.0f);
                    });
                    prop_row("Ambient", [&] {
                        color_edit_linear("##AmbientColor", dl->ambient_color);
                    });
                    prop_row("Ambient Int.", [&] {
                        ImGui::DragFloat("##AmbientIntensity", &dl->ambient_intensity, 0.05f, 0.0f, 2.0f);
                    });
                }
            }
            if (auto* pl = dynamic_cast<PointLightNode*>(obj)) {
                section_header("POINT LIGHT", ICON_LC_LIGHTBULB);
                if (auto t = PropertyTable("##ptlight")) {
                    prop_row("Color", [&] {
                        color_edit_linear("##PLColor", pl->color);
                    });
                    prop_row("Intensity", [&] {
                        ImGui::DragFloat("##PLIntensity", &pl->intensity, 0.1f, 0.0f, 100.0f);
                    });
                    prop_row("Radius", [&] {
                        ImGui::DragFloat("##PLRadius", &pl->radius, 0.5f, 0.1f, 500.0f);
                    });
                    prop_row("Vol. Scatter", [&] {
                        ImGui::DragFloat("##PLVolScatter", &pl->vol_scatter_intensity, 0.05f, 0.0f, 10.0f);
                    });
                }
                ImGui::Checkbox("Cast Shadows##PL", &pl->cast_shadows);
                if (pl->cast_shadows) {
                    if (auto t = PropertyTable("##ptlight_shadow")) {
                        prop_row("Shadow Bias", [&] {
                            ImGui::DragFloat("##PLShadowBias", &pl->shadow_bias, 0.0005f, 0.0f, 0.05f, "%.4f");
                        });
                    }
                }
            }
            if (auto* sl = dynamic_cast<SpotLightNode*>(obj)) {
                section_header("SPOT LIGHT", ICON_LC_FLASHLIGHT);
                if (auto t = PropertyTable("##spot")) {
                    prop_row("Direction", [&] {
                        ImGui::DragFloat3("##SLDir", &sl->direction.x, 0.01f, -1.0f, 1.0f);
                    });
                    prop_row("Color", [&] {
                        color_edit_linear("##SLColor", sl->color);
                    });
                    prop_row("Intensity", [&] {
                        ImGui::DragFloat("##SLIntensity", &sl->intensity, 0.1f, 0.0f, 100.0f);
                    });
                    prop_row("Radius", [&] {
                        ImGui::DragFloat("##SLRadius", &sl->radius, 0.5f, 0.1f, 500.0f);
                    });
                    prop_row("Inner Angle", [&] {
                        ImGui::DragFloat("##SLInner", &sl->inner_angle, 0.5f, 0.0f, 90.0f);
                    });
                    prop_row("Outer Angle", [&] {
                        ImGui::DragFloat("##SLOuter", &sl->outer_angle, 0.5f, 0.0f, 90.0f);
                    });
                    prop_row("Vol. Scatter", [&] {
                        ImGui::DragFloat("##SLVolScatter", &sl->vol_scatter_intensity, 0.05f, 0.0f, 10.0f);
                    });
                }
                ImGui::Checkbox("Cast Shadows##SL", &sl->cast_shadows);
                if (sl->cast_shadows) {
                    if (auto t = PropertyTable("##spot_shadow")) {
                        prop_row("Shadow Bias", [&] {
                            ImGui::DragFloat("##SLShadowBias", &sl->shadow_bias, 0.0005f, 0.0f, 0.05f, "%.4f");
                        });
                    }
                }
            }

            if (auto* vf = dynamic_cast<VolumetricFogNode*>(obj)) {
                section_header("VOLUMETRIC FOG", ICON_LC_CLOUD);
                if (auto t = PropertyTable("##fog")) {
                    prop_row("Density", [&] {
                        ImGui::SliderFloat("##FogDensity", &vf->density, 0.001f, 0.2f, "%.4f");
                    });
                    prop_row("Height Falloff", [&] {
                        ImGui::SliderFloat("##FogFalloff", &vf->height_falloff, 0.01f, 5.0f);
                    });
                    prop_row("Scattering", [&] {
                        ImGui::SliderFloat("##FogScatter", &vf->scattering, 0.0f, 1.0f);
                    });
                    prop_row("Anisotropy", [&] {
                        ImGui::SliderFloat("##FogAniso", &vf->anisotropy, -0.99f, 0.99f);
                    });
                    prop_row("Max Distance", [&] {
                        ImGui::SliderFloat("##FogDist", &vf->max_distance, 10.0f, 1000.0f);
                    });
                    prop_row("Temporal", [&] {
                        ImGui::SliderFloat("##FogTemporal", &vf->temporal_blend, 0.0f, 0.99f);
                    });
                    prop_row("Color", [&] {
                        color_edit_linear("##FogColor", vf->fog_color);
                    });
                }
                ImGui::Checkbox("Sun Shafts##FogShafts", &vf->shafts_enabled);
                if (vf->shafts_enabled) {
                    if (auto t = PropertyTable("##fog_shafts")) {
                        prop_row("Intensity", [&] {
                            ImGui::SliderFloat("##ShaftIntensity", &vf->shaft_intensity, 0.0f, 10.0f);
                        });
                        prop_row("Density Mult", [&] {
                            ImGui::SliderFloat("##ShaftDensity", &vf->shaft_density_mult, 0.0f, 5.0f);
                        });
                        prop_row("Max Reach", [&] {
                            ImGui::SliderFloat("##ShaftReach", &vf->shaft_max_reach, 10.0f, 1000.0f);
                        });
                        prop_row("Steps", [&] {
                            ImGui::SliderInt("##ShaftSteps", &vf->shaft_step_count, 8, 128);
                        });
                        prop_row("Anisotropy", [&] {
                            ImGui::SliderFloat("##ShaftAniso", &vf->shaft_anisotropy, 0.0f, 0.9f);
                        });
                        prop_row("Directionality", [&] {
                            ImGui::SliderFloat("##ShaftDirectionality", &vf->shaft_directionality, 0.0f, 1.0f);
                        });
                        prop_row("Threshold", [&] {
                            ImGui::SliderFloat("##ShaftThreshold", &vf->shaft_threshold, 0.0f, 0.95f);
                        });
                        prop_row("Halo Inner", [&] {
                            ImGui::SliderFloat("##ShaftHaloInner", &vf->shaft_halo_inner, 0.0f, 1.0f);
                        });
                        prop_row("Halo Outer", [&] {
                            ImGui::SliderFloat("##ShaftHaloOuter", &vf->shaft_halo_outer, -1.0f, 1.0f);
                        });
                    }
                }
            }

            if (auto* env = dynamic_cast<WorldEnvironmentNode*>(obj)) {
                section_header("WORLD ENVIRONMENT", ICON_LC_GLOBE);
                static const std::string hdri_exts[] = {".hdr", ".exr"};
                asset_ref_field({.label = "HDRI",
                                 .id = "##EnvHDRI",
                                 .extensions = hdri_exts,
                                 .ctx = &ictx},
                                env->hdri_path);

                static const std::string sky_shader_exts[] = {".fjsl"};
                asset_ref_field({.label = "Sky Shader",
                                 .id = "##EnvSkyShader",
                                 .extensions = sky_shader_exts,
                                 .ctx = &ictx},
                                env->sky_shader);
                if (!env->sky_shader.empty()) {
                    draw_sky_uniforms(*env, ictx);
                }

                static const std::string day_profile_exts[] = {".fjday"};
                asset_ref_field({.label = "Day Profile",
                                 .id = "##EnvDayProfile",
                                 .extensions = day_profile_exts,
                                 .ctx = &ictx},
                                env->day_profile);

                const bool gradient_mode = env->hdri_path.empty();
                ImGui::BeginDisabled(!gradient_mode);
                if (auto t = PropertyTable("##env_sky")) {
                    prop_row("Sky Top", [&] {
                        color_edit_linear("##SkyTop", env->sky_top_color);
                    });
                    prop_row("Sky Bottom", [&] {
                        color_edit_linear("##SkyBottom", env->sky_bottom_color);
                    });
                }
                ImGui::EndDisabled();
                if (!gradient_mode) {
                    ImGui::TextDisabled("(gradient overridden by HDRI)");
                }

                if (auto t = PropertyTable("##env_ibl")) {
                    prop_row("IBL Intensity", [&] {
                        ImGui::SliderFloat("##IBLIntensity", &env->ibl_intensity, 0.0f, 5.0f);
                    });
                }

                ImGui::Checkbox("Contact Shadows##EnvContact", &env->contact_shadows_enabled);
                if (env->contact_shadows_enabled) {
                    if (auto t = PropertyTable("##env_contact")) {
                        prop_row("Distance", [&] {
                            ImGui::SliderFloat("##ContactDist", &env->contact_shadow_distance, 0.05f, 2.0f);
                        });
                        prop_row("Thickness", [&] {
                            ImGui::SliderFloat("##ContactThick", &env->contact_shadow_thickness, 0.01f, 0.5f);
                        });
                        prop_row("Steps", [&] {
                            ImGui::SliderInt("##ContactSteps", &env->contact_shadow_steps, 4, 64);
                        });
                    }
                }

                ImGui::Checkbox("Time of Day##EnvTod", &env->time_of_day_enabled);
                if (env->time_of_day_enabled) {
                    if (auto t = PropertyTable("##env_tod")) {
                        prop_row("Hour", [&] {
                            const int minutes = static_cast<int>(env->time_of_day * 60.0f + 0.5f) % (24 * 60);
                            char label[8];
                            std::snprintf(label, sizeof(label), "%02d:%02d", minutes / 60, minutes % 60);
                            ImGui::SliderFloat("##TodHour", &env->time_of_day, 0.0f, 24.0f, label);
                        });
                        prop_row("Day Length (min)", [&] {
                            float minutes = env->day_length / 60.0f;
                            if (ImGui::DragFloat("##TodDayLength", &minutes, 0.5f, 0.0f, 24.0f * 60.0f, "%.1f")) {
                                env->day_length = std::max(minutes, 0.0f) * 60.0f;
                            }
                        });
                        prop_row("Run in Editor", [&] {
                            ImGui::Checkbox("##TodAdvance", &env->advance_in_editor);
                        });
                        prop_row("Path Tilt", [&] {
                            ImGui::SliderFloat("##TodTilt", &env->sun_path_tilt, 0.0f, 90.0f, "%.0f°");
                        });
                        prop_row("North", [&] {
                            ImGui::SliderFloat("##TodNorth", &env->north_yaw, -180.0f, 180.0f, "%.0f°");
                        });
                        prop_row("Noon Hour", [&] {
                            ImGui::SliderFloat("##TodNoon", &env->noon_hour, 0.0f, 24.0f, "%.1f");
                        });
                    }
                    ImGui::TextDisabled("Moon");
                    if (auto t = PropertyTable("##env_moon")) {
                        prop_row("Hours Behind Sun", [&] {
                            ImGui::SliderFloat("##MoonOffset", &env->moon_offset_hours, 0.0f, 24.0f, "%.1f");
                        });
                        prop_row("Phase", [&] {
                            ImGui::SliderFloat("##MoonPhase", &env->moon_phase, 0.0f, 1.0f, "%.2f");
                            if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 and 1 are new, 0.5 is full");
                        });
                        prop_row("Color", [&] {
                            color_edit_linear("##MoonColor", env->moon_color);
                        });
                        prop_row("Intensity", [&] {
                            ImGui::DragFloat("##MoonIntensity", &env->moon_intensity, 0.005f, 0.0f, 2.0f, "%.3f");
                        });
                    }
                }

                ImGui::Checkbox("Atmospheric Scattering##EnvAtmos", &env->atmosphere_enabled);
                if (env->atmosphere_enabled) {
                    if (auto t = PropertyTable("##env_atmos")) {
                        prop_row("Sun Intensity", [&] {
                            ImGui::SliderFloat("##AtmosSunI", &env->atmosphere_sun_intensity, 1.0f, 50.0f);
                        });
                        prop_row("Rayleigh H (m)", [&] {
                            ImGui::SliderFloat("##AtmosRayleighScale", &env->atmosphere_rayleigh_scale, 1000.0f, 20000.0f);
                        });
                        prop_row("Mie H (m)", [&] {
                            ImGui::SliderFloat("##AtmosMieScale", &env->atmosphere_mie_scale, 100.0f, 5000.0f);
                        });
                        prop_row("Mie Aniso.", [&] {
                            ImGui::SliderFloat("##AtmosMieG", &env->atmosphere_mie_g, 0.0f, 0.99f);
                        });
                        prop_row("Mie Coeff.", [&] {
                            ImGui::SliderFloat("##AtmosMieCoeff", &env->atmosphere_mie_coeff, 1e-6f, 100e-6f, "%.1e");
                        });
                        prop_row("HDRI Floor", [&] {
                            ImGui::SliderFloat("##AtmosHdriFloor", &env->atmosphere_hdri_floor, 0.0f, 1.0f);
                        });
                    }
                }
            }

            // Add Component popup — lists types not already present
            if (ImGui::Button(ICON_LC_PLUS "  Add Component", {-1, 0})) {
                ImGui::OpenPopup("##AddBuiltinComponent");
            }
            if (ImGui::BeginPopup("##AddBuiltinComponent")) {
                registry.for_each([&](ComponentTypeId id, const ComponentMeta& meta) {
                    if (!obj->components.get_by_id(id) && ImGui::MenuItem(meta.display_name.c_str())) {
                        meta.init(*obj, ictx);
                        scene_->mark_dirty();
                    }
                });
                ImGui::EndPopup();
            }

            // Scripts
            section_header("SCRIPTS", ICON_LC_FILE_CODE);
            scripts_->draw_inspector(*obj);

            // Add script button
            if (scripts_->is_loaded() && !scripts_->script_types().empty()) {
                if (ImGui::Button(ICON_LC_PLUS "  Add Script", {-1, 0})) {
                    ImGui::OpenPopup("##AddScript");
                }
                if (ImGui::BeginPopup("##AddScript")) {
                    for (const auto& type : scripts_->script_types()) {
                        if (type.loaded && ImGui::MenuItem(type.name.c_str())) {
                            if (auto r = scripts_->add_script(*obj, type.name); !r) {
                                FJELL_CORE_ERROR("{}", r.error());
                            }
                        }
                    }
                    ImGui::EndPopup();
                }
            }
            } // if (obj) — spatial node sections
        } else {
            ImGui::TextDisabled("No object selected");
        }

    }
}

void PropertiesPanel::draw_material_inspector_for(Material* mat) {
    if (!mat || !pipeline_cache_ || !bindless_) return;

    uint32_t pid = mat->pipeline_id();
    if (pid >= pipeline_cache_->shader_count()) return;

    const auto& meta = pipeline_cache_->metadata(pid);

    // Only show if shader has custom uniforms
    bool has_custom = false;
    for (const auto& u : meta.uniforms) {
        if (u.type != UniformType::Sampler2D) {
            has_custom = true;
            break;
        }
    }

    ImGui::TextDisabled(ICON_LC_FILE_CODE "  Shader: %s", mat->shader_path().c_str());

    // UV scale: every surface material has one; a ui quad's UV is its own and
    // a terrain material tiles each layer by that layer's own scale
    if (meta.shader_type == "spatial") {
        const auto& gpu = bindless_->material_at(mat->material_id());
        float uv_sc[2] = {gpu.base_params[7], gpu.base_params[8]};
        if (uv_sc[0] == 0.0f) uv_sc[0] = 1.0f;
        if (uv_sc[1] == 0.0f) uv_sc[1] = 1.0f;
        if (auto t = PropertyTable("##mat_uv")) {
            prop_row("UV Scale", [&] {
                if (ImGui::DragFloat2("##UVScale", uv_sc, 0.1f, 0.01f, 100.0f)) {
                    GpuMaterial updated = gpu;
                    updated.base_params[7] = uv_sc[0];
                    updated.base_params[8] = uv_sc[1];
                    bindless_->update_material(mat->material_id(), updated);
                    scene_->mark_dirty();
                }
            });
        }
    }

    if (!has_custom) return;

    if (draw_material_params(meta, *bindless_, *mat)) {
        scene_->mark_dirty();
    }
}

void PropertiesPanel::draw_material_inspector(Node3D& obj) {
    auto* mesh = obj.components.get<MeshComponent>();
    if (!mesh) return;
    draw_material_inspector_for(mesh->material);
}

} // namespace fjell
