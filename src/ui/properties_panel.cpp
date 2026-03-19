#include "ui/properties_panel.hpp"
#include "core/command_history.hpp"
#include "core/commands.hpp"
#include "scene/scene.hpp"
#include "renderer/bindless.hpp"
#include "renderer/fjsl_parser.hpp"
#include "renderer/material.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/uniform_types.hpp"
#include "renderer/particle_emitter.hpp"
#include "renderer/sprite_desc.hpp"
#include "renderer/pass/grid_pass.hpp"
#include "physics/physics_types.hpp"
#include "scripting/script_manager.hpp"

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
                           BindlessTextures* bindless) {
    scene_ = scene;
    selected_id_ = selected_id;
    gizmo_op_ = gizmo_op;
    light_ = light;
    scripts_ = scripts;
    history_ = history;
    grid_settings_ = grid_settings;
    pipeline_cache_ = pipeline_cache;
    bindless_ = bindless;
}

void PropertiesPanel::draw() {
    if (ImGui::Begin("Properties")) {
        auto* obj = scene_->find(*selected_id_);
        if (obj) {

            // Editable name
            char name_buf[128];
            std::strncpy(name_buf, obj->name.c_str(), sizeof(name_buf) - 1);
            name_buf[sizeof(name_buf) - 1] = '\0';
            std::string old_name = obj->name;
            ImGui::Text("Name");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##Name", name_buf, sizeof(name_buf))) {
                obj->name = name_buf;
                scene_->mark_dirty();
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && history_ && obj->name != old_name) {
                history_->execute(std::make_unique<RenameCommand>(
                    *scene_, obj->id, old_name, obj->name));
            }

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

            // Mesh
            if (obj->mesh) {
                if (ImGui::CollapsingHeader("Mesh", ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::TextDisabled("Model: %s", obj->source.empty() ? "(none)" : obj->source.c_str());
                    ImGui::TextDisabled("Material: %s", obj->mesh->material_name.c_str());
                    draw_material_inspector(*obj);
                    if (ImGui::SmallButton("Remove Mesh")) {
                        obj->mesh.reset();
                        scene_->mark_dirty();
                    }
                }
            }

            // Rigidbody
            if (obj->rigidbody) {
                if (ImGui::CollapsingHeader("Rigidbody", ImGuiTreeNodeFlags_DefaultOpen)) {
                    auto& rb = *obj->rigidbody;
                    static const char* body_type_labels[] = {"Static", "Kinematic", "Dynamic"};
                    int bt = static_cast<int>(rb.body_type);
                    ImGui::Text("Body Type");
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::Combo("##BodyType", &bt, body_type_labels, 3)) {
                        rb.body_type = static_cast<BodyType>(bt);
                    }
                    ImGui::Text("Mass");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::DragFloat("##Mass", &rb.mass, 0.1f, 0.01f, 1000.0f);
                    ImGui::Text("Friction");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::DragFloat("##Friction", &rb.friction, 0.01f, 0.0f, 2.0f);
                    ImGui::Text("Restitution");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::DragFloat("##Restitution", &rb.restitution, 0.01f, 0.0f, 2.0f);
                    ImGui::Text("Linear Damping");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::DragFloat("##LinearDamping", &rb.linear_damping, 0.01f, 0.0f, 10.0f);
                    ImGui::Text("Angular Damping");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::DragFloat("##AngularDamping", &rb.angular_damping, 0.01f, 0.0f, 10.0f);
                    ImGui::Checkbox("Gravity", &rb.gravity_enabled);
                    if (ImGui::SmallButton("Remove Rigidbody")) {
                        obj->rigidbody.reset();
                    }
                }
            }

            // Collider
            if (obj->collider) {
                if (ImGui::CollapsingHeader("Collider", ImGuiTreeNodeFlags_DefaultOpen)) {
                    auto& col = *obj->collider;
                    static const char* shape_labels[] = {"Box", "Sphere", "Capsule", "Mesh"};
                    int sh = static_cast<int>(col.shape);
                    ImGui::Text("Shape");
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::Combo("##Shape", &sh, shape_labels, 4)) {
                        col.shape = static_cast<ColliderShape>(sh);
                    }
                    switch (col.shape) {
                        case ColliderShape::Box:
                            ImGui::Text("Half Extents");
                            ImGui::SetNextItemWidth(-1);
                            ImGui::DragFloat3("##HalfExtents", &col.half_extents.x, 0.05f, 0.01f, 100.0f);
                            break;
                        case ColliderShape::Sphere:
                            ImGui::Text("Radius");
                            ImGui::SetNextItemWidth(-1);
                            ImGui::DragFloat("##Radius", &col.radius, 0.05f, 0.01f, 100.0f);
                            break;
                        case ColliderShape::Capsule:
                            ImGui::Text("Radius");
                            ImGui::SetNextItemWidth(-1);
                            ImGui::DragFloat("##RadiusCap", &col.radius, 0.05f, 0.01f, 100.0f);
                            ImGui::Text("Height");
                            ImGui::SetNextItemWidth(-1);
                            ImGui::DragFloat("##Height", &col.height, 0.05f, 0.01f, 100.0f);
                            break;
                        case ColliderShape::Mesh:
                            ImGui::TextDisabled("Uses model collision mesh");
                            break;
                    }
                    ImGui::Text("Offset");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::DragFloat3("##Offset", &col.offset.x, 0.05f);
                    if (ImGui::SmallButton("Remove Collider")) {
                        obj->collider.reset();
                    }
                }
            }

            // Particle Emitter
            if (obj->emitter) {
                if (ImGui::CollapsingHeader("Particle Emitter", ImGuiTreeNodeFlags_DefaultOpen)) {
                    auto& em = *obj->emitter;
                    if (ImGui::DragFloat("Emit Rate", &em.emission_rate, 1.0f, 0.0f, 10000.0f)) scene_->mark_dirty();
                    if (ImGui::DragFloat("Lifetime Min", &em.lifetime_min, 0.1f, 0.01f, 30.0f)) scene_->mark_dirty();
                    if (ImGui::DragFloat("Lifetime Max", &em.lifetime_max, 0.1f, 0.01f, 30.0f)) scene_->mark_dirty();
                    int max_p = static_cast<int>(em.max_particles);
                    if (ImGui::DragInt("Max Particles", &max_p, 100, 100, 100000)) {
                        em.max_particles = static_cast<uint32_t>(max_p);
                        scene_->mark_dirty();
                    }
                    if (ImGui::DragFloat3("Velocity Min", &em.velocity_min.x, 0.1f)) scene_->mark_dirty();
                    if (ImGui::DragFloat3("Velocity Max", &em.velocity_max.x, 0.1f)) scene_->mark_dirty();
                    if (ImGui::DragFloat3("Gravity", &em.gravity.x, 0.1f)) scene_->mark_dirty();
                    if (ImGui::ColorEdit4("Color Start", &em.color_start.r)) scene_->mark_dirty();
                    if (ImGui::ColorEdit4("Color End", &em.color_end.r)) scene_->mark_dirty();
                    if (ImGui::DragFloat("Size Start", &em.size_start, 0.01f, 0.001f, 10.0f)) scene_->mark_dirty();
                    if (ImGui::DragFloat("Size End", &em.size_end, 0.01f, 0.001f, 10.0f)) scene_->mark_dirty();
                    static const char* blend_labels[] = {"Additive", "Alpha"};
                    int bm = static_cast<int>(em.blend_mode);
                    if (ImGui::Combo("Blend Mode", &bm, blend_labels, 2)) {
                        em.blend_mode = static_cast<ParticleBlendMode>(bm);
                        scene_->mark_dirty();
                    }
                    if (ImGui::SmallButton("Remove Emitter")) {
                        obj->emitter.reset();
                        scene_->mark_dirty();
                    }
                }
            }

            // Sprite
            if (obj->sprite) {
                if (ImGui::CollapsingHeader("Sprite", ImGuiTreeNodeFlags_DefaultOpen)) {
                    auto& sp = *obj->sprite;

                    char tex_buf[256];
                    std::strncpy(tex_buf, sp.texture_path.c_str(), sizeof(tex_buf) - 1);
                    tex_buf[sizeof(tex_buf) - 1] = '\0';
                    ImGui::Text("Texture");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::InputText("##SpriteTexture", tex_buf, sizeof(tex_buf));
                    if (ImGui::IsItemDeactivatedAfterEdit()) {
                        sp.texture_path = tex_buf;
                        sp.texture_loaded = false;
                        scene_->mark_dirty();
                    }

                    ImGui::Text("Tint");
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::ColorEdit4("##SpriteTint", &sp.tint.r)) scene_->mark_dirty();

                    ImGui::Text("Opacity");
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::DragFloat("##SpriteOpacity", &sp.opacity, 0.01f, 0.0f, 1.0f)) scene_->mark_dirty();

                    if (ImGui::SmallButton("Remove Sprite")) {
                        obj->sprite.reset();
                        scene_->mark_dirty();
                    }
                }
            }

            // Add Component popup — lists types not already present
            if (ImGui::Button("Add Component", {-1, 0})) {
                ImGui::OpenPopup("##AddBuiltinComponent");
            }
            if (ImGui::BeginPopup("##AddBuiltinComponent")) {
                if (!obj->rigidbody && ImGui::MenuItem("Rigidbody")) {
                    obj->rigidbody = RigidbodyDesc{};
                    scene_->mark_dirty();
                }
                if (!obj->collider && ImGui::MenuItem("Collider")) {
                    ColliderDesc col{};
                    if (obj->source == "primitive:cube") {
                        col.shape = ColliderShape::Box;
                    } else if (obj->source == "primitive:sphere") {
                        col.shape = ColliderShape::Sphere;
                        col.radius = 0.5f;
                    } else {
                        col.shape = ColliderShape::Box;
                    }
                    obj->collider = col;
                    scene_->mark_dirty();
                }
                if (!obj->emitter && ImGui::MenuItem("Particle Emitter")) {
                    obj->emitter = ParticleEmitter{};
                    scene_->mark_dirty();
                }
                if (!obj->sprite && ImGui::MenuItem("Sprite")) {
                    obj->sprite = SpriteDesc{};
                    scene_->mark_dirty();
                }
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
                            scripts_->add_script(*obj, type.name);
                        }
                    }
                    ImGui::EndPopup();
                }
            }
        } else {
            ImGui::TextDisabled("No object selected");
        }

        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Directional Light", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Direction");
            ImGui::SetNextItemWidth(-1);
            ImGui::DragFloat3("##Direction", &light_->direction.x, 0.01f, -1.0f, 1.0f);
            ImGui::Text("Color");
            ImGui::SetNextItemWidth(-1);
            ImGui::ColorEdit3("##Color", &light_->color.x);
            ImGui::Text("Intensity");
            ImGui::SetNextItemWidth(-1);
            ImGui::DragFloat("##Intensity", &light_->intensity, 0.05f, 0.0f, 10.0f);
            ImGui::Spacing();
            ImGui::Text("Ambient");
            ImGui::SetNextItemWidth(-1);
            ImGui::ColorEdit3("##Ambient", &light_->ambient_color.x);
            ImGui::Text("Ambient Intensity");
            ImGui::SetNextItemWidth(-1);
            ImGui::DragFloat("##AmbientIntensity", &light_->ambient_intensity, 0.05f, 0.0f, 2.0f);
        }

        if (ImGui::CollapsingHeader("Point Lights", ImGuiTreeNodeFlags_DefaultOpen)) {
            auto& point_lights = scene_->point_lights();
            int remove_idx = -1;
            for (int i = 0; i < static_cast<int>(point_lights.size()); ++i) {
                ImGui::PushID(i);
                auto& pl = point_lights[i];
                bool open = ImGui::TreeNode("##pl", "Point Light %d", i);
                if (open) {
                    ImGui::DragFloat3("Position", &pl.position.x, 0.1f);
                    ImGui::ColorEdit3("Color", &pl.color.x);
                    ImGui::DragFloat("Intensity", &pl.intensity, 0.1f, 0.0f, 100.0f);
                    ImGui::DragFloat("Radius", &pl.radius, 0.5f, 0.1f, 500.0f);
                    if (ImGui::SmallButton("Remove")) remove_idx = i;
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            if (remove_idx >= 0) {
                point_lights.erase(point_lights.begin() + remove_idx);
                scene_->mark_dirty();
            }
            if (ImGui::Button("Add Point Light", {-1, 0})) {
                scene_->add_point_light();
            }
        }

        if (ImGui::CollapsingHeader("Spot Lights")) {
            auto& spot_lights = scene_->spot_lights();
            int remove_idx = -1;
            for (int i = 0; i < static_cast<int>(spot_lights.size()); ++i) {
                ImGui::PushID(1000 + i);
                auto& sl = spot_lights[i];
                bool open = ImGui::TreeNode("##sl", "Spot Light %d", i);
                if (open) {
                    ImGui::DragFloat3("Position", &sl.position.x, 0.1f);
                    ImGui::DragFloat3("Direction", &sl.direction.x, 0.01f, -1.0f, 1.0f);
                    ImGui::ColorEdit3("Color", &sl.color.x);
                    ImGui::DragFloat("Intensity", &sl.intensity, 0.1f, 0.0f, 100.0f);
                    ImGui::DragFloat("Radius", &sl.radius, 0.5f, 0.1f, 500.0f);
                    ImGui::DragFloat("Inner Angle", &sl.inner_angle, 0.5f, 0.0f, 90.0f);
                    ImGui::DragFloat("Outer Angle", &sl.outer_angle, 0.5f, 0.0f, 90.0f);
                    if (ImGui::SmallButton("Remove")) remove_idx = i;
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            if (remove_idx >= 0) {
                spot_lights.erase(spot_lights.begin() + remove_idx);
                scene_->mark_dirty();
            }
            if (ImGui::Button("Add Spot Light", {-1, 0})) {
                scene_->add_spot_light();
            }
        }
    }
    ImGui::End();
}

void PropertiesPanel::draw_material_inspector(SceneNode& obj) {
    if (!obj.mesh || !obj.mesh->material || !pipeline_cache_ || !bindless_) return;

    auto* mat = obj.mesh->material;
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

    uint32_t param_count = mat->custom_param_count();
    if (param_count == 0) return;

    bool changed = false;

    for (const auto& u : meta.uniforms) {
        if (u.type == UniformType::Sampler2D) continue;

        uint32_t fcount = uniform_float_count(u.type);
        if (u.slot + fcount > param_count) continue;

        uint32_t abs_offset = gpu.param_offset + u.slot;

        ImGui::PushID(u.name.c_str());
        ImGui::Text("%s", u.name.c_str());
        ImGui::SetNextItemWidth(-1);

        // Read current values from CPU-side params buffer
        float temp[4]{};
        for (uint32_t i = 0; i < fcount; ++i) {
            temp[i] = bindless_->read_param(abs_offset + i);
        }

        switch (u.type) {
            case UniformType::Float:
                if (u.hint == UniformHint::None && u.range.max > u.range.min) {
                    if (ImGui::SliderFloat("##val", temp, u.range.min, u.range.max)) {
                        bindless_->write_params(abs_offset, temp, 1);
                        changed = true;
                    }
                } else {
                    if (ImGui::DragFloat("##val", temp, 0.01f)) {
                        bindless_->write_params(abs_offset, temp, 1);
                        changed = true;
                    }
                }
                break;

            case UniformType::Vec3:
                if (u.hint == UniformHint::Color) {
                    if (ImGui::ColorEdit3("##val", temp)) {
                        bindless_->write_params(abs_offset, temp, 3);
                        changed = true;
                    }
                } else {
                    if (ImGui::DragFloat3("##val", temp, 0.01f)) {
                        bindless_->write_params(abs_offset, temp, 3);
                        changed = true;
                    }
                }
                break;

            case UniformType::Vec4:
                if (u.hint == UniformHint::Color) {
                    if (ImGui::ColorEdit4("##val", temp)) {
                        bindless_->write_params(abs_offset, temp, 4);
                        changed = true;
                    }
                } else {
                    if (ImGui::DragFloat4("##val", temp, 0.01f)) {
                        bindless_->write_params(abs_offset, temp, 4);
                        changed = true;
                    }
                }
                break;

            case UniformType::Vec2:
                if (ImGui::DragFloat2("##val", temp, 0.01f)) {
                    bindless_->write_params(abs_offset, temp, 2);
                    changed = true;
                }
                break;

            default:
                break;
        }

        ImGui::PopID();
    }

    if (changed) {
        scene_->mark_dirty();
    }
}

} // namespace fjell
