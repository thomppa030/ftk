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
#include "renderer/uniform_types.hpp"
#include "scene/camera_node.hpp"
#include "scene/component_registry.hpp"
#include "scene/light_node.hpp"
#include "scene/scene.hpp"
#include "renderer/pass/grid_pass.hpp"
#include "scripting/script_manager.hpp"
#include "ui/icon_cache.hpp"
#include "ui/inspector_widgets.hpp"
#include "ui/material_params_widget.hpp"

#include <imgui.h>
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

void PropertiesPanel::init(Scene* scene, uint32_t* selected_id, int* gizmo_op,
                           DirectionalLight* light, ScriptManager* scripts,
                           CommandHistory* history, GridSettings* grid_settings,
                           PipelineCache* pipeline_cache,
                           BindlessTextures* bindless,
                           ResourceManager* resource_manager,
                           ProjectManager* project_mgr) {
    scene_ = scene;
    selected_id_ = selected_id;
    gizmo_op_ = gizmo_op;
    light_ = light;
    scripts_ = scripts;
    history_ = history;
    grid_settings_ = grid_settings;
    pipeline_cache_ = pipeline_cache;
    bindless_ = bindless;
    resource_manager_ = resource_manager;
    project_mgr_ = project_mgr;
}

void PropertiesPanel::draw() {
    if (ImGui::Begin("Properties")) {
        if (auto* ic = IconCache::instance()) ic->draw_panel_icon("component");
        auto* base = scene_->find(*selected_id_);
        auto* obj = base ? base->as_spatial() : nullptr;
        if (base) {

            // Editable name
            char name_buf[128];
            std::strncpy(name_buf, base->name.c_str(), sizeof(name_buf) - 1);
            name_buf[sizeof(name_buf) - 1] = '\0';
            std::string old_name = base->name;
            ImGui::Text("Name");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##Name", name_buf, sizeof(name_buf))) {
                base->name = name_buf;
                scene_->mark_dirty();
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && history_ && base->name != old_name) {
                history_->execute(std::make_unique<RenameCommand>(
                    *scene_, base->id, old_name, base->name));
            }

            if (obj) {
            ImGui::Separator();
            ImGui::TextColored({0.345f, 0.396f, 0.949f, 1.0f}, "Transform");
            ImGui::Text("Position");
            if (grid_settings_ && grid_settings_->snap_enabled) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Snap")) {
                    Transform before = obj->transform;
                    obj->transform.position = snap_to_grid(obj->transform.position, grid_settings_->cell_size);
                    scene_->mark_dirty();
                    if (history_) {
                        history_->execute(std::make_unique<TransformCommand>(
                            *scene_, obj->id, before, obj->transform, obj->name));
                    }
                }
            }
            float drag_speed = (grid_settings_ && grid_settings_->snap_enabled)
                ? grid_settings_->cell_size : 0.1f;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat3("##Position", &obj->transform.position.x, drag_speed)) {
                if (grid_settings_ && grid_settings_->snap_enabled) {
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
            ImGui::Text("Rotation");
            ImGui::SetNextItemWidth(-1);
            {
                glm::vec3 euler = obj->transform.euler_degrees();
                if (ImGui::DragFloat3("##Rotation", &euler.x, 1.0f)) {
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
            }
            ImGui::Text("Scale");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat3("##Scale", &obj->transform.scale.x, 0.05f, 0.01f, 100.0f)) scene_->mark_dirty();
            if (ImGui::IsItemActivated()) {
                drag_start_transform_ = obj->transform;
                drag_start_name_ = obj->name;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && history_) {
                history_->execute(std::make_unique<TransformCommand>(
                    *scene_, obj->id, drag_start_transform_, obj->transform, obj->name));
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextColored({0.345f, 0.396f, 0.949f, 1.0f}, "Gizmo");
            ImGui::RadioButton("Move (W)", gizmo_op_, 0); ImGui::SameLine();
            ImGui::RadioButton("Rot (E)", gizmo_op_, 1); ImGui::SameLine();
            ImGui::RadioButton("Scl (R)", gizmo_op_, 2);

            ImGui::Spacing();
            if (!obj->source.empty()) {
                ImGui::TextDisabled("Source: %s", obj->source.c_str());
            }

            // ── Components ────────────────────────────────────────────
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextColored({0.345f, 0.396f, 0.949f, 1.0f}, "Components");

            const InspectorContext ictx{
                scene_, history_, resource_manager_, pipeline_cache_, bindless_, project_mgr_};

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
                colored_separator({0.31f, 0.79f, 0.63f, 1.0f});
                if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
                    int proj_mode = static_cast<int>(cam->projection);
                    const char* proj_modes[] = {"Perspective", "Orthographic"};
                    ImGui::Text("Projection");
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::Combo("##CamProjection", &proj_mode, proj_modes, 2)) {
                        cam->projection = static_cast<CameraProjection>(proj_mode);
                        scene_->mark_dirty();
                    }
                    if (cam->projection == CameraProjection::perspective) {
                        ImGui::Text("FOV");
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::DragFloat("##CamFOV", &cam->fov, 0.5f, 1.0f, 179.0f))
                            scene_->mark_dirty();
                    } else {
                        ImGui::Text("Ortho Size");
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::DragFloat("##CamOrthoSize", &cam->ortho_size, 0.1f, 0.1f, 1000.0f))
                            scene_->mark_dirty();
                    }
                    ImGui::Text("Near Plane");
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::DragFloat("##CamNear", &cam->near_plane, 0.01f, 0.001f, 100.0f))
                        scene_->mark_dirty();
                    ImGui::Text("Far Plane");
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::DragFloat("##CamFar", &cam->far_plane, 10.0f, 1.0f, 100000.0f))
                        scene_->mark_dirty();
                }
            }

            // Light properties
            if (auto* dl = dynamic_cast<DirectionalLightNode*>(obj)) {
                colored_separator({0.96f, 0.78f, 0.26f, 1.0f});
                if (ImGui::CollapsingHeader("Directional Light", ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::Text("Direction");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::DragFloat3("##LightDir", &dl->direction.x, 0.01f, -1.0f, 1.0f);
                    ImGui::Text("Color");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::ColorEdit3("##LightColor", &dl->color.x);
                    ImGui::Text("Intensity");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::DragFloat("##LightIntensity", &dl->intensity, 0.05f, 0.0f, 10.0f);
                    ImGui::Spacing();
                    ImGui::Text("Ambient");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::ColorEdit3("##AmbientColor", &dl->ambient_color.x);
                    ImGui::Text("Ambient Intensity");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::DragFloat("##AmbientIntensity", &dl->ambient_intensity, 0.05f, 0.0f, 2.0f);
                }
            }
            if (auto* pl = dynamic_cast<PointLightNode*>(obj)) {
                colored_separator({0.96f, 0.78f, 0.26f, 1.0f});
                if (ImGui::CollapsingHeader("Point Light", ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::Text("Color");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::ColorEdit3("##PLColor", &pl->color.x);
                    ImGui::Text("Intensity");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::DragFloat("##PLIntensity", &pl->intensity, 0.1f, 0.0f, 100.0f);
                    ImGui::Text("Radius");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::DragFloat("##PLRadius", &pl->radius, 0.5f, 0.1f, 500.0f);
                }
            }
            if (auto* sl = dynamic_cast<SpotLightNode*>(obj)) {
                colored_separator({0.96f, 0.78f, 0.26f, 1.0f});
                if (ImGui::CollapsingHeader("Spot Light", ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::Text("Direction");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::DragFloat3("##SLDir", &sl->direction.x, 0.01f, -1.0f, 1.0f);
                    ImGui::Text("Color");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::ColorEdit3("##SLColor", &sl->color.x);
                    ImGui::Text("Intensity");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::DragFloat("##SLIntensity", &sl->intensity, 0.1f, 0.0f, 100.0f);
                    ImGui::Text("Radius");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::DragFloat("##SLRadius", &sl->radius, 0.5f, 0.1f, 500.0f);
                    ImGui::Text("Inner Angle");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::DragFloat("##SLInner", &sl->inner_angle, 0.5f, 0.0f, 90.0f);
                    ImGui::Text("Outer Angle");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::DragFloat("##SLOuter", &sl->outer_angle, 0.5f, 0.0f, 90.0f);
                }
            }

            if (auto* vf = dynamic_cast<VolumetricFogNode*>(obj)) {
                colored_separator({0.55f, 0.70f, 0.85f, 1.0f});
                if (ImGui::CollapsingHeader("Volumetric Fog", ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::Text("Density");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::SliderFloat("##FogDensity", &vf->density, 0.001f, 0.2f, "%.4f");
                    ImGui::Text("Height Falloff");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::SliderFloat("##FogFalloff", &vf->height_falloff, 0.01f, 5.0f);
                    ImGui::Text("Scattering");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::SliderFloat("##FogScatter", &vf->scattering, 0.0f, 1.0f);
                    ImGui::Text("Anisotropy (Godrays)");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::SliderFloat("##FogAniso", &vf->anisotropy, -0.99f, 0.99f);
                    ImGui::Text("Max Distance");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::SliderFloat("##FogDist", &vf->max_distance, 10.0f, 1000.0f);
                    ImGui::Text("Temporal Blend");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::SliderFloat("##FogTemporal", &vf->temporal_blend, 0.0f, 0.99f);
                    ImGui::Text("Fog Color");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::ColorEdit3("##FogColor", &vf->fog_color.r);
                }
            }

            if (auto* env = dynamic_cast<WorldEnvironmentNode*>(obj)) {
                colored_separator({0.4f, 0.6f, 0.9f, 1.0f});
                if (ImGui::CollapsingHeader("World Environment", ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::Text("Sky Top Color");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::ColorEdit3("##SkyTop", &env->sky_top_color.x);
                    ImGui::Text("Sky Bottom Color");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::ColorEdit3("##SkyBottom", &env->sky_bottom_color.x);
                    ImGui::Text("IBL Intensity");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::SliderFloat("##IBLIntensity", &env->ibl_intensity, 0.0f, 5.0f);
                }
            }

            // Add Component popup — lists types not already present
            if (ImGui::Button("Add Component", {-1, 0})) {
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
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextColored({0.345f, 0.396f, 0.949f, 1.0f}, "Scripts");
            scripts_->draw_inspector(*obj);

            // Add script button
            if (scripts_->is_loaded() && !scripts_->script_types().empty()) {
                if (ImGui::Button("Add Script", {-1, 0})) {
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
    ImGui::End();
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

    ImGui::TextDisabled("Shader: %s", mat->shader_path().c_str());

    // UV scale (available on all materials)
    const auto& gpu = bindless_->material_at(mat->material_id());
    float uv_sc[2] = {gpu.base_params[7], gpu.base_params[8]};
    if (uv_sc[0] == 0.0f) uv_sc[0] = 1.0f;
    if (uv_sc[1] == 0.0f) uv_sc[1] = 1.0f;
    ImGui::Text("UV Scale");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::DragFloat2("##UVScale", uv_sc, 0.1f, 0.01f, 100.0f)) {
        GpuMaterial updated = gpu;
        updated.base_params[7] = uv_sc[0];
        updated.base_params[8] = uv_sc[1];
        bindless_->update_material(mat->material_id(), updated);
        scene_->mark_dirty();
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
