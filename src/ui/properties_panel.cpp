#include "ui/properties_panel.hpp"
#include "core/base_path.hpp"
#include "core/command_history.hpp"
#include "core/commands.hpp"
#include "core/log.hpp"
#include "core/project_manager.hpp"
#include "renderer/bindless.hpp"
#include "renderer/resources/fjsl_parser.hpp"
#include "renderer/resources/material.hpp"
#include "renderer/pipeline_cache.hpp"
#include "environment/environment_state.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/uniform_types.hpp"
#include "scene/camera_node.hpp"
#include "scene/component_registry.hpp"
#include "scene/light_node.hpp"
#include "environment/environment_resolver.hpp"
#include "environment/weather_asset.hpp"
#include "environment/environment_subsystem.hpp"
#include "scene/scene.hpp"
#include "renderer/pass/grid_pass.hpp"
#include "scripting/script_manager.hpp"
#include "ui/icon_cache.hpp"
#include "ui/icons_lc.hpp"
#include "ui/asset_ref_widget.hpp"
#include "renderer/resources/asset_shader_type.hpp"
#include "ui/inspector_widgets.hpp"
#include "ui/kit/text_field.hpp"
#include "renderer/pass/sky_pass.hpp"
#include "core/string_utils.hpp"
#include "ui/material_params_widget.hpp"
#include "ui/panel_widget.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <filesystem>
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
    if (meta.shader_type != ShaderType::Sky) return;

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

// The fog material's uniforms as rows on a Volumetric Fog or Fog Volume node:
// each shows the node's own value when it has one and the material's
// otherwise, and an edit stores the node's value in `params`. A day profile or
// weather track of the same name folds over both while it is bound.
static void draw_fog_overrides(const Material* material, std::string_view default_shader,
                               std::vector<NamedValue>& params,
                               const std::vector<NamedValue>& material_values,
                               const InspectorContext& ictx) {
    if (ictx.pipeline_cache == nullptr) return;
    const uint32_t pid = material != nullptr
        ? material->pipeline_id()
        : ictx.pipeline_cache->register_shader(std::string(default_shader));
    if (pid >= ictx.pipeline_cache->shader_count()
        || ictx.pipeline_cache->has_compile_error(pid)) return;
    const auto& meta = ictx.pipeline_cache->metadata(pid);
    if (meta.shader_type != ShaderType::Fog) return;

    bool any = false;
    for (const auto& u : meta.uniforms) any |= u.type != UniformType::Sampler2D;
    if (!any) return;

    ImGui::TextDisabled("This Node");
    ImGui::SetItemTooltip("Values of the material's uniforms for this node alone. "
                          "The material keeps its own; other nodes sharing it are untouched.");
    for (const auto& u : meta.uniforms) {
        if (u.type == UniformType::Sampler2D) continue;
        const uint32_t count = uniform_float_count(u.type);
        NamedValue* override = find_named(params, u.name);
        const NamedValue* base = find_named(material_values, u.name);

        std::array<float, 4> temp{};
        for (uint32_t i = 0; i < count && i < u.default_value.size(); ++i) temp[i] = u.default_value[i];
        if (base != nullptr) {
            for (uint32_t i = 0; i < count && i < base->count; ++i) temp[i] = base->values[i];
        }
        if (override != nullptr) {
            for (uint32_t i = 0; i < count && i < override->count; ++i) temp[i] = override->values[i];
        }

        ImGui::PushID(u.name.c_str());
        ImGui::Text("%s", display_name(u.name).c_str());
        if (override != nullptr) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset")) {
                params.erase(params.begin() + (override - params.data()));
                if (ictx.scene != nullptr) ictx.scene->mark_dirty();
                ImGui::PopID();
                continue;
            }
        }
        ImGui::SetNextItemWidth(-1);
        if (draw_uniform_value(u, std::span{temp}.first(count))) {
            NamedValue value;
            value.name = u.name;
            value.count = count;
            value.values = temp;
            value.srgb = u.hint == UniformHint::Color;
            set_named(params, std::move(value));
            if (ictx.scene != nullptr) ictx.scene->mark_dirty();
        }
        ImGui::PopID();
    }
}

// Below the Weather reference: a pick list of the project's events, and
// what is playing right now with Stop and Replay. Setting the field starts
// the event; the pick list is the same assignment without a file dialog.
void PropertiesPanel::draw_weather_controls(WorldEnvironmentNode& env, const InspectorContext& ictx) {
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
                if (scene_ != nullptr) scene_->mark_dirty();
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
                            if (scene_ != nullptr) scene_->mark_dirty();
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

    if (environment_ == nullptr || ictx.resource_manager == nullptr) return;
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

void PropertiesPanel::commit_name() {
    auto* node = scene_->find(name_node_id_);
    const std::string& new_name = name_field_.text();
    if (node == nullptr || new_name.empty() || new_name == node->name) return;
    if (history_) {
        history_->execute(std::make_unique<RenameCommand>(
            *scene_, node->id, node->name, new_name));
    } else {
        node->name = new_name;
        scene_->mark_dirty();
    }
}

void PropertiesPanel::draw() {
    if (auto p = Panel(ICON_LC_SLIDERS_HORIZONTAL, "Properties")) {
        auto* base = scene_->find(*selected_id_);
        auto* obj = base ? base->as_spatial() : nullptr;
        if (base) {

            // Editable name: one undoable rename, on Enter or when the
            // field loses focus. The node is taken when the edit starts,
            // since the click that ends it can select another node first.
            if (!name_field_.editing()) {
                name_node_id_ = base->id;
            }
            const Node* named = scene_->find(name_node_id_);
            if (named == nullptr) {
                named = base;
                name_node_id_ = base->id;
            }
            if (auto t = PropertyTable("##name")) {
                prop_row("Name", [&] {
                    if (name_field_.draw("##Name", named->name).committed) {
                        commit_name();
                    }
                });
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

                // Each component's widgets get their own ID scope: two
                // components on one node may both have an "Add" or "Clear".
                ImGui::PushID(meta->json_key.c_str());
                colored_separator(meta->color);
                if (ImGui::CollapsingHeader(meta->display_name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                    if (meta->draw(*obj, ptr, ictx)) {
                        scene_->mark_dirty();
                    }

                    // A component that paints itself with a material shows
                    // that material's own parameters here, so a surface is
                    // tuned where it is placed.
                    if (meta->material != nullptr) {
                        draw_material_inspector_for(meta->material(ptr));
                    }

                    std::string remove_label = "Remove " + meta->display_name;
                    if (remove_button(remove_label.c_str())) {
                        remove_id = id;
                    }
                }
                ImGui::PopID();
            });

            if (remove_id != ~0u) {
                if (history_) {
                    const auto* meta = registry.find(remove_id);
                    history_->execute(std::make_unique<RemoveComponentCommand>(
                        *scene_, obj->id, remove_id, meta ? meta->display_name : "component"));
                } else {
                    obj->components.remove_by_id(remove_id);
                    scene_->mark_dirty();
                }
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
                        if (ImGui::DragFloat3("##LightDir", &dl->direction.x, 0.01f, -1.0f, 1.0f)) scene_->mark_dirty();
                        ImGui::EndDisabled();
                        if (sun_driven && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                            ImGui::SetTooltip("Driven by the World Environment's time of day");
                        }
                    });
                    prop_row("Color", [&] {
                        if (color_edit_linear("##LightColor", dl->color)) scene_->mark_dirty();
                    });
                    prop_row("Intensity", [&] {
                        if (ImGui::DragFloat("##LightIntensity", &dl->intensity, 0.05f, 0.0f, 10.0f)) scene_->mark_dirty();
                    });
                    prop_row("Ambient", [&] {
                        if (color_edit_linear("##AmbientColor", dl->ambient_color)) scene_->mark_dirty();
                    });
                    prop_row("Ambient Int.", [&] {
                        if (ImGui::DragFloat("##AmbientIntensity", &dl->ambient_intensity, 0.05f, 0.0f, 2.0f)) scene_->mark_dirty();
                    });
                }
            }
            if (auto* pl = dynamic_cast<PointLightNode*>(obj)) {
                section_header("POINT LIGHT", ICON_LC_LIGHTBULB);
                if (auto t = PropertyTable("##ptlight")) {
                    prop_row("Color", [&] {
                        if (color_edit_linear("##PLColor", pl->color)) scene_->mark_dirty();
                    });
                    prop_row("Intensity", [&] {
                        if (ImGui::DragFloat("##PLIntensity", &pl->intensity, 0.1f, 0.0f, 100.0f)) scene_->mark_dirty();
                    });
                    prop_row("Radius", [&] {
                        if (ImGui::DragFloat("##PLRadius", &pl->radius, 0.5f, 0.1f, 500.0f)) scene_->mark_dirty();
                    });
                    prop_row("Vol. Scatter", [&] {
                        if (ImGui::DragFloat("##PLVolScatter", &pl->vol_scatter_intensity, 0.05f, 0.0f, 10.0f)) scene_->mark_dirty();
                    });
                }
                if (ImGui::Checkbox("Cast Shadows##PL", &pl->cast_shadows)) scene_->mark_dirty();
                if (pl->cast_shadows) {
                    if (auto t = PropertyTable("##ptlight_shadow")) {
                        prop_row("Shadow Bias", [&] {
                            if (ImGui::DragFloat("##PLShadowBias", &pl->shadow_bias, 0.0005f, 0.0f, 0.05f, "%.4f")) scene_->mark_dirty();
                        });
                    }
                }
            }
            if (auto* sl = dynamic_cast<SpotLightNode*>(obj)) {
                section_header("SPOT LIGHT", ICON_LC_FLASHLIGHT);
                if (auto t = PropertyTable("##spot")) {
                    prop_row("Direction", [&] {
                        if (ImGui::DragFloat3("##SLDir", &sl->direction.x, 0.01f, -1.0f, 1.0f)) scene_->mark_dirty();
                    });
                    prop_row("Color", [&] {
                        if (color_edit_linear("##SLColor", sl->color)) scene_->mark_dirty();
                    });
                    prop_row("Intensity", [&] {
                        if (ImGui::DragFloat("##SLIntensity", &sl->intensity, 0.1f, 0.0f, 100.0f)) scene_->mark_dirty();
                    });
                    prop_row("Radius", [&] {
                        if (ImGui::DragFloat("##SLRadius", &sl->radius, 0.5f, 0.1f, 500.0f)) scene_->mark_dirty();
                    });
                    prop_row("Inner Angle", [&] {
                        if (ImGui::DragFloat("##SLInner", &sl->inner_angle, 0.5f, 0.0f, 90.0f)) scene_->mark_dirty();
                    });
                    prop_row("Outer Angle", [&] {
                        if (ImGui::DragFloat("##SLOuter", &sl->outer_angle, 0.5f, 0.0f, 90.0f)) scene_->mark_dirty();
                    });
                    prop_row("Vol. Scatter", [&] {
                        if (ImGui::DragFloat("##SLVolScatter", &sl->vol_scatter_intensity, 0.05f, 0.0f, 10.0f)) scene_->mark_dirty();
                    });
                }
                if (ImGui::Checkbox("Cast Shadows##SL", &sl->cast_shadows)) scene_->mark_dirty();
                if (sl->cast_shadows) {
                    if (auto t = PropertyTable("##spot_shadow")) {
                        prop_row("Shadow Bias", [&] {
                            if (ImGui::DragFloat("##SLShadowBias", &sl->shadow_bias, 0.0005f, 0.0f, 0.05f, "%.4f")) scene_->mark_dirty();
                        });
                    }
                }
            }

            if (auto* vf = dynamic_cast<VolumetricFogNode*>(obj)) {
                section_header("VOLUMETRIC FOG", ICON_LC_CLOUD);
                // How the haze looks is its material; the node keeps where it
                // is, how far it reaches and how it answers movement.
                static const std::string fog_mat_exts[] = {".fjmat"};
                if (asset_ref_field({.label = "Material",
                                     .id = "##FogMatRef",
                                     .extensions = fog_mat_exts,
                                     .ctx = &ictx,
                                     .shader_types = only_shader_type(ShaderType::Fog)},
                                    vf->material_path)) {
                    scene_->mark_dirty();
                }
                if (vf->material != nullptr) {
                    draw_material_inspector_for(vf->material);
                } else {
                    ImGui::TextDisabled(ICON_LC_FILE_CODE "  Shader: %s (engine default)",
                                        std::string(DEFAULT_FOG_SHADER).c_str());
                }
                draw_fog_overrides(vf->material, DEFAULT_FOG_SHADER, vf->params,
                                   vf->material_values_, ictx);
                if (auto t = PropertyTable("##fog")) {
                    prop_row("Max Distance", [&] {
                        if (ImGui::SliderFloat("##FogDist", &vf->max_distance, 10.0f, 1000.0f)) scene_->mark_dirty();
                    });
                    prop_row("Temporal", [&] {
                        if (ImGui::SliderFloat("##FogTemporal", &vf->temporal_blend, 0.0f, 0.99f)) scene_->mark_dirty();
                    });
                }
                if (ImGui::Checkbox("Sun Shafts##FogShafts", &vf->shafts_enabled)) scene_->mark_dirty();
                if (vf->shafts_enabled) {
                    if (auto t = PropertyTable("##fog_shafts")) {
                        prop_row("Intensity", [&] {
                            if (ImGui::SliderFloat("##ShaftIntensity", &vf->shaft_intensity, 0.0f, 10.0f)) scene_->mark_dirty();
                        });
                        prop_row("Density Mult", [&] {
                            if (ImGui::SliderFloat("##ShaftDensity", &vf->shaft_density_mult, 0.0f, 5.0f)) scene_->mark_dirty();
                        });
                        prop_row("Max Reach", [&] {
                            if (ImGui::SliderFloat("##ShaftReach", &vf->shaft_max_reach, 10.0f, 1000.0f)) scene_->mark_dirty();
                        });
                        prop_row("Steps", [&] {
                            if (ImGui::SliderInt("##ShaftSteps", &vf->shaft_step_count, 8, 128)) scene_->mark_dirty();
                        });
                        prop_row("Anisotropy", [&] {
                            if (ImGui::SliderFloat("##ShaftAniso", &vf->shaft_anisotropy, 0.0f, 0.9f)) scene_->mark_dirty();
                        });
                        prop_row("Directionality", [&] {
                            if (ImGui::SliderFloat("##ShaftDirectionality", &vf->shaft_directionality, 0.0f, 1.0f)) scene_->mark_dirty();
                        });
                        prop_row("Threshold", [&] {
                            if (ImGui::SliderFloat("##ShaftThreshold", &vf->shaft_threshold, 0.0f, 0.95f)) scene_->mark_dirty();
                        });
                    }
                }

                section_header("MOTION", ICON_LC_AUDIO_WAVEFORM);
                if (auto t = PropertyTable("##fog_motion")) {
                    prop_row("Response", [&] {
                        if (ImGui::SliderFloat("##FogMotion", &vf->motion_response, 0.0f, 1.0f)) {
                            scene_->mark_dirty();
                        }
                    });
                    prop_row("Show Field", [&] {
                        bool on = vf->density_field_debug > 0.0f;
                        if (ImGui::Checkbox("##FogFieldDebug", &on)) {
                            vf->density_field_debug = on ? 1.0f : 0.0f;
                            scene_->mark_dirty();
                        }
                    });
                }
                if (vf->density_field_debug > 0.0f) {
                    ImGui::TextDisabled("Red = air pushed aside, blue = piled up.");
                }
                if (vf->motion_response <= 0.0f) {
                    ImGui::TextDisabled("Ignores things moving through it.");
                } else {
                    ImGui::TextDisabled("Faint in open haze — a few metres of a\n"
                                        "long column. Fog volumes carry a wake.");
                }
            }

            if (auto* fv = dynamic_cast<FogVolumeNode*>(obj)) {
                section_header("FOG VOLUME", ICON_LC_CLOUD);
                // What the bank looks like is its material; the node keeps
                // the shape, how the bank drifts and how it answers movement.
                static const std::string volume_mat_exts[] = {".fjmat"};
                if (asset_ref_field({.label = "Material",
                                     .id = "##FogVolMatRef",
                                     .extensions = volume_mat_exts,
                                     .ctx = &ictx,
                                     .shader_types = only_shader_type(ShaderType::Fog)},
                                    fv->material_path)) {
                    scene_->mark_dirty();
                }
                if (fv->material != nullptr) {
                    draw_material_inspector_for(fv->material);
                } else {
                    ImGui::TextDisabled(ICON_LC_FILE_CODE "  Shader: %s (engine default)",
                                        std::string(DEFAULT_FOG_VOLUME_SHADER).c_str());
                }
                draw_fog_overrides(fv->material, DEFAULT_FOG_VOLUME_SHADER, fv->params,
                                   fv->material_values_, ictx);
                if (auto t = PropertyTable("##fog_volume")) {
                    prop_row("Shape", [&] {
                        int shape = static_cast<int>(fv->shape);
                        const char* shapes[] = {"Box", "Sphere"};
                        if (ImGui::Combo("##FogVolShape", &shape, shapes, 2)) {
                            fv->shape = static_cast<ColliderShape>(shape);
                            scene_->mark_dirty();
                        }
                    });
                    prop_row("Edge Falloff", [&] {
                        if (ImGui::SliderFloat("##FogVolFalloff", &fv->edge_falloff, 0.0f, 1.0f)) {
                            scene_->mark_dirty();
                        }
                    });
                    prop_row("Group", [&] {
                        char group_buf[64];
                        std::snprintf(group_buf, sizeof(group_buf), "%s", fv->fog_group.c_str());
                        if (ImGui::InputText("##FogVolGroup", group_buf, sizeof(group_buf))) {
                            fv->fog_group = group_buf;
                        }
                        if (ImGui::IsItemDeactivatedAfterEdit()) scene_->mark_dirty();
                    });
                }
                ImGui::TextDisabled("Size comes from the node's scale.");
                if (!fv->fog_group.empty()) {
                    ImGui::TextDisabled("Driven by fog.%s.<uniform> tracks.", fv->fog_group.c_str());
                }

                // How the bank's field moves through the world. What the
                // field is, the wisps' size and contrast, is the material's.
                section_header("DRIFT", ICON_LC_AUDIO_WAVEFORM);
                if (auto t = PropertyTable("##fog_volume_drift")) {
                    prop_row("Direction", [&] {
                        if (ImGui::DragFloat3("##FogVolDriftDir", &fv->drift_direction.x, 0.01f, -1.0f, 1.0f, "%.2f")) {
                            scene_->mark_dirty();
                        }
                    });
                    prop_row("Speed", [&] {
                        if (ImGui::SliderFloat("##FogVolDriftSpeed", &fv->drift_speed, 0.0f, 5.0f, "%.2f m/s")) {
                            scene_->mark_dirty();
                        }
                    });
                    prop_row("Wind", [&] {
                        if (ImGui::SliderFloat("##FogVolWind", &fv->wind_influence, 0.0f, 1.0f)) {
                            scene_->mark_dirty();
                        }
                    });
                    if (fv->wind_influence > 0.0f) {
                        prop_row("Wind Scale", [&] {
                            if (ImGui::SliderFloat("##FogVolWindScale", &fv->wind_speed_scale, 0.0f, 1.0f, "%.2f")) {
                                scene_->mark_dirty();
                            }
                        });
                    }
                }

                section_header("MOTION", ICON_LC_AUDIO_WAVEFORM);
                if (auto t = PropertyTable("##fog_volume_motion")) {
                    prop_row("Response", [&] {
                        if (ImGui::SliderFloat("##FogVolMotion", &fv->motion_response, 0.0f, 1.0f)) {
                            scene_->mark_dirty();
                        }
                    });
                    prop_row("Vortices", [&] {
                        if (ImGui::SliderFloat("##FogVolVortex", &fv->vortex_strength,
                                               0.0f, 2.0f)) {
                            scene_->mark_dirty();
                        }
                    });
                    prop_row("Compression", [&] {
                        if (ImGui::SliderFloat("##FogVolCompress", &fv->compression,
                                               0.0f, 3.0f)) {
                            scene_->mark_dirty();
                        }
                    });
                    prop_row("Show Field", [&] {
                        bool on = fv->density_field_debug > 0.0f;
                        if (ImGui::Checkbox("##FogVolFieldDebug", &on)) {
                            fv->density_field_debug = on ? 1.0f : 0.0f;
                            scene_->mark_dirty();
                        }
                    });
                }
                if (fv->motion_response <= 0.0f) {
                    ImGui::TextDisabled("Ignores things moving through it.");
                } else if (fv->vortex_strength > 0.0f) {
                    ImGui::TextDisabled("Rolls shed off each flank of a wake.");
                }
                if (fv->density_field_debug > 0.0f) {
                    ImGui::TextDisabled("Red = air pushed aside, blue = piled up.");
                }
            }

            if (auto* env = dynamic_cast<WorldEnvironmentNode*>(obj)) {
                section_header("WORLD ENVIRONMENT", ICON_LC_GLOBE);
                static const std::string hdri_exts[] = {".hdr", ".exr"};
                if (asset_ref_field({.label = "HDRI",
                                     .id = "##EnvHDRI",
                                     .extensions = hdri_exts,
                                     .ctx = &ictx},
                                    env->hdri_path)) {
                    scene_->mark_dirty();
                }

                static const std::string sky_shader_exts[] = {".fjsl"};
                if (asset_ref_field({.label = "Sky Shader",
                                     .id = "##EnvSkyShader",
                                     .extensions = sky_shader_exts,
                                     .ctx = &ictx,
                                     .shader_types = only_shader_type(ShaderType::Sky)},
                                    env->sky_shader)) {
                    scene_->mark_dirty();
                }
                if (!env->sky_shader.empty()) {
                    draw_sky_uniforms(*env, ictx);
                }

                static const std::string day_profile_exts[] = {".fjday"};
                if (asset_ref_field({.label = "Day Profile",
                                     .id = "##EnvDayProfile",
                                     .extensions = day_profile_exts,
                                     .ctx = &ictx},
                                    env->day_profile)) {
                    scene_->mark_dirty();
                }

                static const std::string weather_exts[] = {".fjweather"};
                if (asset_ref_field({.label = "Weather",
                                     .id = "##EnvWeather",
                                     .extensions = weather_exts,
                                     .ctx = &ictx},
                                    env->weather)) {
                    scene_->mark_dirty();
                }
                static const std::string climate_exts[] = {".fjclimate"};
                if (asset_ref_field({.label = "Climate",
                                     .id = "##EnvClimate",
                                     .extensions = climate_exts,
                                     .ctx = &ictx},
                                    env->climate)) {
                    scene_->mark_dirty();
                }
                draw_weather_controls(*env, ictx);

                const bool gradient_mode = env->hdri_path.empty();
                ImGui::BeginDisabled(!gradient_mode);
                if (auto t = PropertyTable("##env_sky")) {
                    prop_row("Sky Top", [&] {
                        if (color_edit_linear("##SkyTop", env->sky_top_color)) scene_->mark_dirty();
                    });
                    prop_row("Sky Bottom", [&] {
                        if (color_edit_linear("##SkyBottom", env->sky_bottom_color)) scene_->mark_dirty();
                    });
                }
                ImGui::EndDisabled();
                if (!gradient_mode) {
                    ImGui::TextDisabled("(gradient overridden by HDRI)");
                }

                if (auto t = PropertyTable("##env_ibl")) {
                    prop_row("IBL Intensity", [&] {
                        if (ImGui::SliderFloat("##IBLIntensity", &env->ibl_intensity, 0.0f, 5.0f)) scene_->mark_dirty();
                    });
                }

                if (ImGui::Checkbox("Contact Shadows##EnvContact", &env->contact_shadows_enabled)) scene_->mark_dirty();
                if (env->contact_shadows_enabled) {
                    if (auto t = PropertyTable("##env_contact")) {
                        prop_row("Distance", [&] {
                            if (ImGui::SliderFloat("##ContactDist", &env->contact_shadow_distance, 0.05f, 2.0f)) scene_->mark_dirty();
                        });
                        prop_row("Thickness", [&] {
                            if (ImGui::SliderFloat("##ContactThick", &env->contact_shadow_thickness, 0.01f, 0.5f)) scene_->mark_dirty();
                        });
                        prop_row("Steps", [&] {
                            if (ImGui::SliderInt("##ContactSteps", &env->contact_shadow_steps, 4, 64)) scene_->mark_dirty();
                        });
                    }
                }

                if (ImGui::Checkbox("Time of Day##EnvTod", &env->time_of_day_enabled)) scene_->mark_dirty();
                if (env->time_of_day_enabled) {
                    if (auto t = PropertyTable("##env_tod")) {
                        prop_row("Hour", [&] {
                            const int minutes = static_cast<int>(env->time_of_day * 60.0f + 0.5f) % (24 * 60);
                            char label[8];
                            std::snprintf(label, sizeof(label), "%02d:%02d", minutes / 60, minutes % 60);
                            if (ImGui::SliderFloat("##TodHour", &env->time_of_day, 0.0f, 24.0f, label)) scene_->mark_dirty();
                        });
                        prop_row("Day Length (min)", [&] {
                            float minutes = env->day_length / 60.0f;
                            if (ImGui::DragFloat("##TodDayLength", &minutes, 0.5f, 0.0f, 24.0f * 60.0f, "%.1f")) {
                                env->day_length = std::max(minutes, 0.0f) * 60.0f;
                            }
                        });
                        prop_row("Run in Editor", [&] {
                            if (ImGui::Checkbox("##TodAdvance", &env->advance_in_editor)) scene_->mark_dirty();
                        });
                        prop_row("Path Tilt", [&] {
                            if (ImGui::SliderFloat("##TodTilt", &env->sun_path_tilt, 0.0f, 90.0f, "%.0f°")) scene_->mark_dirty();
                        });
                        prop_row("North", [&] {
                            if (ImGui::SliderFloat("##TodNorth", &env->north_yaw, -180.0f, 180.0f, "%.0f°")) scene_->mark_dirty();
                        });
                        prop_row("Noon Hour", [&] {
                            if (ImGui::SliderFloat("##TodNoon", &env->noon_hour, 0.0f, 24.0f, "%.1f")) scene_->mark_dirty();
                        });
                    }
                    ImGui::TextDisabled("Moon");
                    if (auto t = PropertyTable("##env_moon")) {
                        prop_row("Phase", [&] {
                            // Named on the slider, because a quarter moon is
                            // the half-lit one and a number alone invites
                            // reading 0.5 as half.
                            const float phase = env->moon_phase;
                            const char* name = phase < 0.06F || phase > 0.94F ? "new"
                                             : phase < 0.19F ? "waxing crescent"
                                             : phase < 0.31F ? "first quarter (half)"
                                             : phase < 0.44F ? "waxing gibbous"
                                             : phase < 0.56F ? "full"
                                             : phase < 0.69F ? "waning gibbous"
                                             : phase < 0.81F ? "last quarter (half)"
                                                             : "waning crescent";
                            if (ImGui::SliderFloat("##MoonPhase", &env->moon_phase, 0.0f, 1.0f, name)) scene_->mark_dirty();
                            if (ImGui::IsItemHovered()) {
                                ImGui::SetTooltip(
                                    "Where the moon rides the sun's path, and so how much of it is\n"
                                    "lit: new at 0 and 1, half at 0.25 and 0.75, full at 0.5.");
                            }
                        });
                        prop_row("Color", [&] {
                            if (color_edit_linear("##MoonColor", env->moon_color)) scene_->mark_dirty();
                        });
                        prop_row("Intensity", [&] {
                            if (ImGui::DragFloat("##MoonIntensity", &env->moon_intensity, 0.005f, 0.0f, 2.0f, "%.3f")) scene_->mark_dirty();
                        });
                    }
                }

                if (ImGui::Checkbox("Atmospheric Scattering##EnvAtmos", &env->atmosphere_enabled)) scene_->mark_dirty();
                if (env->atmosphere_enabled) {
                    if (auto t = PropertyTable("##env_atmos")) {
                        prop_row("Sun Intensity", [&] {
                            if (ImGui::SliderFloat("##AtmosSunI", &env->atmosphere_sun_intensity, 1.0f, 50.0f)) scene_->mark_dirty();
                        });
                        prop_row("Rayleigh H (m)", [&] {
                            if (ImGui::SliderFloat("##AtmosRayleighScale", &env->atmosphere_rayleigh_scale, 1000.0f, 20000.0f)) scene_->mark_dirty();
                        });
                        prop_row("Mie H (m)", [&] {
                            if (ImGui::SliderFloat("##AtmosMieScale", &env->atmosphere_mie_scale, 100.0f, 5000.0f)) scene_->mark_dirty();
                        });
                        prop_row("Mie Aniso.", [&] {
                            if (ImGui::SliderFloat("##AtmosMieG", &env->atmosphere_mie_g, 0.0f, 0.99f)) scene_->mark_dirty();
                        });
                        prop_row("Mie Coeff.", [&] {
                            if (ImGui::SliderFloat("##AtmosMieCoeff", &env->atmosphere_mie_coeff, 1e-6f, 100e-6f, "%.1e")) scene_->mark_dirty();
                        });
                        prop_row("HDRI Floor", [&] {
                            if (ImGui::SliderFloat("##AtmosHdriFloor", &env->atmosphere_hdri_floor, 0.0f, 1.0f)) scene_->mark_dirty();
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
            if (scripts_->draw_inspector(*obj)) {
                scene_->mark_dirty();
            }

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
    if (meta.shader_type == ShaderType::Spatial) {
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

} // namespace fjell
