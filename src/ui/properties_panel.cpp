#include "ui/properties_panel.hpp"
#include "audio/audio_component.hpp"
#include "core/input_component.hpp"
#include "core/command_history.hpp"
#include "core/commands.hpp"
#include "core/log.hpp"
#include "renderer/arrow_component.hpp"
#include "renderer/bindless.hpp"
#include "renderer/fjsl_parser.hpp"
#include "renderer/material.hpp"
#include "renderer/mesh_component.hpp"
#include "renderer/particle_emitter.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/uniform_types.hpp"
#include "scene/scene.hpp"
#include "renderer/sprite_anim_component.hpp"
#include "renderer/sprite_desc.hpp"
#include "renderer/tilemap_component.hpp"
#include "renderer/pass/grid_pass.hpp"
#include "physics/physics_types.hpp"
#include "scene/camera_node.hpp"
#include "ui/game_ui/ui_widget_component.hpp"
#include "scene/light_node.hpp"
#include "scripting/script_manager.hpp"
#include "ui/icon_cache.hpp"
#include "ui/material_params_widget.hpp"

#include <imgui.h>
#include <glm/gtc/type_ptr.hpp>

#include <cmath>
#include <cstring>

namespace fjell {

namespace {

void colored_separator(const ImVec4& color) {
    ImGui::Spacing();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddRectFilled(
        p, {p.x + w, p.y + 2.0f},
        ImGui::ColorConvertFloat4ToU32(color));
    ImGui::Dummy({0.0f, 4.0f});
}

// Asset path field: InputText + drag-and-drop target from content browser
// Returns true if the path was changed.
bool asset_path_field(const char* label, const char* id, std::string& path) {
    bool changed = false;
    char buf[256];
    std::strncpy(buf, path.c_str(), sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    ImGui::Text("%s", label);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText(id, buf, sizeof(buf));
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        path = buf;
        changed = true;
    }
    // Drop target for ASSET_PATH from content browser
    if (ImGui::BeginDragDropTarget()) {
        if (const auto* payload = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
            path = std::string(static_cast<const char*>(payload->Data));
            changed = true;
        }
        ImGui::EndDragDropTarget();
    }
    return changed;
}

bool remove_button(const char* label) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button, {0.5f, 0.15f, 0.15f, 0.6f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.7f, 0.2f, 0.2f, 0.8f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0.8f, 0.15f, 0.15f, 1.0f});
    bool pressed = ImGui::Button(label, {-1, 0});
    ImGui::PopStyleColor(3);
    ImGui::Spacing();
    return pressed;
}

} // namespace

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

            // Mesh
            if (auto* mesh = obj->components.get<MeshComponent>()) {
                colored_separator({0.36f, 0.55f, 0.94f, 1.0f});
                if (ImGui::CollapsingHeader("Mesh", ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::TextDisabled("Model: %s", obj->source.empty() ? "(none)" : obj->source.c_str());
                    ImGui::TextDisabled("Material: %s", mesh->material_name.c_str());
                    draw_material_inspector(*obj);
                    if (remove_button("Remove Mesh")) {
                        obj->components.remove<MeshComponent>();
                        scene_->mark_dirty();
                    }
                }
            }

            // Rigidbody
            if (auto* rb = obj->components.get<RigidbodyComponent>()) {
                colored_separator({0.85f, 0.45f, 0.25f, 1.0f});
                if (ImGui::CollapsingHeader("Rigidbody", ImGuiTreeNodeFlags_DefaultOpen)) {
                    static const char* body_type_labels[] = {"Static", "Kinematic", "Dynamic"};
                    int bt = static_cast<int>(rb->body_type);
                    ImGui::Text("Body Type");
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::Combo("##BodyType", &bt, body_type_labels, 3)) {
                        rb->body_type = static_cast<BodyType>(bt);
                    }
                    ImGui::Text("Mass");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::DragFloat("##Mass", &rb->mass, 0.1f, 0.01f, 1000.0f);
                    ImGui::Text("Friction");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::DragFloat("##Friction", &rb->friction, 0.01f, 0.0f, 2.0f);
                    ImGui::Text("Restitution");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::DragFloat("##Restitution", &rb->restitution, 0.01f, 0.0f, 2.0f);
                    ImGui::Text("Linear Damping");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::DragFloat("##LinearDamping", &rb->linear_damping, 0.01f, 0.0f, 10.0f);
                    ImGui::Text("Angular Damping");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::DragFloat("##AngularDamping", &rb->angular_damping, 0.01f, 0.0f, 10.0f);
                    ImGui::Checkbox("Gravity", &rb->gravity_enabled);
                    if (remove_button("Remove Rigidbody")) {
                        obj->components.remove<RigidbodyComponent>();
                    }
                }
            }

            // Character Controller
            if (auto* cc = obj->components.get<CharacterComponent>()) {
                colored_separator({0.2f, 0.8f, 0.8f, 1.0f});
                if (ImGui::CollapsingHeader("Character Controller", ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::Text("Capsule Radius");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::DragFloat("##CharRadius", &cc->capsule_radius, 0.01f, 0.01f, 10.0f);
                    ImGui::Text("Capsule Height");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::DragFloat("##CharHeight", &cc->capsule_height, 0.01f, 0.01f, 20.0f);
                    ImGui::Text("Max Slope Angle");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::DragFloat("##CharSlope", &cc->max_slope_angle, 0.5f, 0.0f, 89.0f);
                    ImGui::Text("Mass");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::DragFloat("##CharMass", &cc->mass, 0.5f, 1.0f, 500.0f);

                    // Read-only ground state display
                    static const char* ground_labels[] = {"On Ground", "On Steep Ground", "Not Supported", "In Air"};
                    ImGui::TextDisabled("Ground: %s", ground_labels[static_cast<int>(cc->ground_state)]);

                    if (remove_button("Remove Character Controller")) {
                        obj->components.remove<CharacterComponent>();
                    }
                }
            }

            // Collider
            if (auto* col = obj->components.get<ColliderComponent>()) {
                colored_separator({0.85f, 0.45f, 0.25f, 1.0f});
                if (ImGui::CollapsingHeader("Collider", ImGuiTreeNodeFlags_DefaultOpen)) {
                    static const char* shape_labels[] = {"Box", "Sphere", "Capsule", "Mesh"};
                    int sh = static_cast<int>(col->shape);
                    ImGui::Text("Shape");
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::Combo("##Shape", &sh, shape_labels, 4)) {
                        col->shape = static_cast<ColliderShape>(sh);
                    }
                    switch (col->shape) {
                        case ColliderShape::Box:
                            ImGui::Text("Half Extents");
                            ImGui::SetNextItemWidth(-1);
                            ImGui::DragFloat3("##HalfExtents", &col->half_extents.x, 0.05f, 0.01f, 100.0f);
                            break;
                        case ColliderShape::Sphere:
                            ImGui::Text("Radius");
                            ImGui::SetNextItemWidth(-1);
                            ImGui::DragFloat("##Radius", &col->radius, 0.05f, 0.01f, 100.0f);
                            break;
                        case ColliderShape::Capsule:
                            ImGui::Text("Radius");
                            ImGui::SetNextItemWidth(-1);
                            ImGui::DragFloat("##RadiusCap", &col->radius, 0.05f, 0.01f, 100.0f);
                            ImGui::Text("Height");
                            ImGui::SetNextItemWidth(-1);
                            ImGui::DragFloat("##Height", &col->height, 0.05f, 0.01f, 100.0f);
                            break;
                        case ColliderShape::Mesh:
                            ImGui::TextDisabled("Uses model collision mesh");
                            break;
                    }
                    ImGui::Text("Offset");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::DragFloat3("##Offset", &col->offset.x, 0.05f);
                    ImGui::Checkbox("Is Trigger", &col->is_trigger);
                    if (remove_button("Remove Collider")) {
                        obj->components.remove<ColliderComponent>();
                    }
                }
            }

            // Particle Emitter
            if (auto* em = obj->components.get<EmitterComponent>()) {
                colored_separator({0.78f, 0.55f, 0.85f, 1.0f});
                if (ImGui::CollapsingHeader("Particle Emitter", ImGuiTreeNodeFlags_DefaultOpen)) {
                    if (ImGui::DragFloat("Emit Rate", &em->emission_rate, 1.0f, 0.0f, 10000.0f)) scene_->mark_dirty();
                    if (ImGui::DragFloat("Lifetime Min", &em->lifetime_min, 0.1f, 0.01f, 30.0f)) scene_->mark_dirty();
                    if (ImGui::DragFloat("Lifetime Max", &em->lifetime_max, 0.1f, 0.01f, 30.0f)) scene_->mark_dirty();
                    int max_p = static_cast<int>(em->max_particles);
                    if (ImGui::DragInt("Max Particles", &max_p, 100, 100, 100000)) {
                        em->max_particles = static_cast<uint32_t>(max_p);
                        scene_->mark_dirty();
                    }
                    if (ImGui::DragFloat3("Velocity Min", &em->velocity_min.x, 0.1f)) scene_->mark_dirty();
                    if (ImGui::DragFloat3("Velocity Max", &em->velocity_max.x, 0.1f)) scene_->mark_dirty();
                    if (ImGui::DragFloat3("Gravity", &em->gravity.x, 0.1f)) scene_->mark_dirty();
                    if (ImGui::ColorEdit4("Color Start", &em->color_start.r)) scene_->mark_dirty();
                    if (ImGui::ColorEdit4("Color End", &em->color_end.r)) scene_->mark_dirty();
                    if (ImGui::DragFloat("Size Start", &em->size_start, 0.01f, 0.001f, 10.0f)) scene_->mark_dirty();
                    if (ImGui::DragFloat("Size End", &em->size_end, 0.01f, 0.001f, 10.0f)) scene_->mark_dirty();
                    static const char* blend_labels[] = {"Additive", "Alpha"};
                    int bm = static_cast<int>(em->blend_mode);
                    if (ImGui::Combo("Blend Mode", &bm, blend_labels, 2)) {
                        em->blend_mode = static_cast<ParticleBlendMode>(bm);
                        scene_->mark_dirty();
                    }
                    if (remove_button("Remove Emitter")) {
                        obj->components.remove<EmitterComponent>();
                        scene_->mark_dirty();
                    }
                }
            }

            // Sprite
            if (auto* sp = obj->components.get<SpriteComponent>()) {
                colored_separator({0.40f, 0.75f, 0.40f, 1.0f});
                if (ImGui::CollapsingHeader("Sprite", ImGuiTreeNodeFlags_DefaultOpen)) {
                    if (asset_path_field("Texture", "##SpriteTexture", sp->texture_path)) {
                        sp->texture_loaded = false;
                        scene_->mark_dirty();
                    }

                    ImGui::Text("Tint");
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::ColorEdit4("##SpriteTint", &sp->tint.r)) scene_->mark_dirty();

                    ImGui::Text("Opacity");
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::DragFloat("##SpriteOpacity", &sp->opacity, 0.01f, 0.0f, 1.0f)) scene_->mark_dirty();

                    if (ImGui::Checkbox("Flat (2D)", &sp->flat)) scene_->mark_dirty();
                    ImGui::SameLine();
                    if (ImGui::Checkbox("Flip H", &sp->flip_h)) scene_->mark_dirty();
                    ImGui::SameLine();
                    if (ImGui::Checkbox("Pixel Art", &sp->pixel_art)) {
                        sp->texture_loaded = false; // re-load with correct sampler
                        scene_->mark_dirty();
                    }

                    ImGui::Text("Pixels Per Unit");
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::DragFloat("##SpritePPU", &sp->pixels_per_unit, 1.0f, 0.0f, 1000.0f, "%.0f"))
                        scene_->mark_dirty();

                    ImGui::Text("Sort Layer / Order");
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
                    if (ImGui::DragInt("##SpriteLayer", &sp->sort_layer, 1.0f)) scene_->mark_dirty();
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::DragInt("##SpriteOrder", &sp->sort_order, 1.0f)) scene_->mark_dirty();

                    if (remove_button("Remove Sprite")) {
                        obj->components.remove<SpriteComponent>();
                        scene_->mark_dirty();
                    }
                }
            }

            // Sprite Animation
            if (auto* sa = obj->components.get<SpriteAnimComponent>()) {
                colored_separator({0.40f, 0.75f, 0.60f, 1.0f});
                if (ImGui::CollapsingHeader("Sprite Animation", ImGuiTreeNodeFlags_DefaultOpen)) {
                    // Animset path (state machine)
                    if (asset_path_field("Animset", "##SpriteAnimset", sa->animset_path)) {
                        sa->loaded = false;
                        scene_->mark_dirty();
                    }

                    ImGui::Separator();
                    ImGui::Text("Legacy Animations");

                    // Legacy animation entries (for scenes without .fjanimset)
                    for (auto& [name, path] : sa->animations) {
                        std::string label = "##SpriteAnim_" + name;
                        if (asset_path_field(name.c_str(), label.c_str(), path)) {
                            sa->loaded = false;
                            scene_->mark_dirty();
                        }
                    }

                    // Add new animation entry
                    static char new_anim_name[64] = "";
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.4f);
                    ImGui::InputText("##NewAnimName", new_anim_name, sizeof(new_anim_name));
                    ImGui::SameLine();
                    if (ImGui::Button("Add Animation") && new_anim_name[0] != '\0') {
                        sa->animations[new_anim_name] = "";
                        sa->loaded = false;
                        scene_->mark_dirty();
                        new_anim_name[0] = '\0';
                    }

                    // Default animation (legacy)
                    ImGui::Text("Default");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(-1);
                    char def_buf[128];
                    std::strncpy(def_buf, sa->default_animation.c_str(), sizeof(def_buf) - 1);
                    def_buf[sizeof(def_buf) - 1] = '\0';
                    if (ImGui::InputText("##SpriteAnimDefault", def_buf, sizeof(def_buf))) {
                        if (ImGui::IsItemDeactivatedAfterEdit()) {
                            sa->default_animation = def_buf;
                            scene_->mark_dirty();
                        }
                    }

                    ImGui::Text("Playback Speed");
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::DragFloat("##SpriteAnimSpeed", &sa->playback_speed, 0.01f, 0.0f, 10.0f))
                        scene_->mark_dirty();

                    if (remove_button("Remove Sprite Animation")) {
                        obj->components.remove<SpriteAnimComponent>();
                        scene_->mark_dirty();
                    }
                }
            }

            // Tilemap
            if (auto* tm = obj->components.get<TilemapComponent>()) {
                colored_separator({0.30f, 0.65f, 0.85f, 1.0f});
                if (ImGui::CollapsingHeader("Tilemap", ImGuiTreeNodeFlags_DefaultOpen)) {
                    if (asset_path_field("Tileset", "##TilemapTileset", tm->tileset_texture)) {
                        tm->texture_loaded = false;
                        tm->dirty = true;
                        scene_->mark_dirty();
                    }

                    ImGui::Text("Tile Size (px)");
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::DragInt2("##TileTileSize", &tm->tile_size.x, 1, 1, 512)) {
                        tm->dirty = true;
                        scene_->mark_dirty();
                    }

                    ImGui::Text("Map Size (tiles)");
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::DragInt2("##TileMapSize", &tm->map_size.x, 1, 1, 256)) {
                        tm->tiles.resize(tm->map_size.x * tm->map_size.y, -1);
                        tm->dirty = true;
                        scene_->mark_dirty();
                    }

                    ImGui::Text("World Tile Size");
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::DragFloat("##TileWorldSize", &tm->world_tile_size, 0.01f, 0.01f, 10.0f)) {
                        tm->dirty = true;
                        scene_->mark_dirty();
                    }

                    ImGui::Text("Sort Layer");
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::DragInt("##TileSortLayer", &tm->sort_layer, 1.0f))
                        scene_->mark_dirty();

                    if (ImGui::Button("Fill All (tile 0)")) {
                        tm->tiles.assign(tm->map_size.x * tm->map_size.y, 0);
                        tm->dirty = true;
                        scene_->mark_dirty();
                    }

                    if (remove_button("Remove Tilemap")) {
                        obj->components.remove<TilemapComponent>();
                        scene_->mark_dirty();
                    }
                }
            }

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

            // UI Widget
            if (auto* wc = obj->components.get<UiWidgetComponent>()) {
                colored_separator({0.3f, 0.85f, 0.75f, 1.0f});
                if (ImGui::CollapsingHeader("UI Widget", ImGuiTreeNodeFlags_DefaultOpen)) {
                    if (asset_path_field("Widget", "##UiWidgetPath", wc->widget_path)) {
                        scene_->mark_dirty();
                    }
                    int mode = static_cast<int>(wc->render_mode);
                    const char* modes[] = {"Screen Space", "World Space"};
                    if (ImGui::Combo("Render Mode", &mode, modes, 2)) {
                        wc->render_mode = static_cast<UiWidgetComponent::RenderMode>(mode);
                        scene_->mark_dirty();
                    }
                    if (wc->render_mode == UiWidgetComponent::RenderMode::world_space) {
                        if (ImGui::DragFloat("Pixels/Unit", &wc->pixels_per_unit, 1.0f, 1.0f, 1000.0f))
                            scene_->mark_dirty();
                        if (ImGui::Checkbox("Billboard", &wc->billboard))
                            scene_->mark_dirty();
                        if (ImGui::DragFloat2("Virtual Viewport", &wc->virtual_viewport.x, 1.0f, 1.0f, 2048.0f))
                            scene_->mark_dirty();
                        if (ImGui::DragFloat3("Offset", &wc->offset.x, 0.05f))
                            scene_->mark_dirty();
                    }
                }
            }

            // Input
            if (obj->components.has<InputComponent>()) {
                auto* ic = obj->components.get<InputComponent>();
                colored_separator({0.4f, 0.9f, 0.5f, 1.0f});
                bool input_open = ImGui::CollapsingHeader("Input", ImGuiTreeNodeFlags_DefaultOpen);
                if (ImGui::BeginPopupContextItem("##InputCtx")) {
                    if (ImGui::MenuItem("Remove")) {
                        obj->components.remove<InputComponent>();
                        scene_->mark_dirty();
                        ic = nullptr;
                    }
                    ImGui::EndPopup();
                }
                if (input_open && ic) {
                    if (asset_path_field("Actions Dir", "##InputActionsDir", ic->actions_dir)) {
                        scene_->mark_dirty();
                    }
                    ImGui::Text("Contexts");
                    int remove_idx = -1;
                    for (int ci = 0; ci < static_cast<int>(ic->context_paths.size()); ++ci) {
                        ImGui::PushID(ci);
                        auto label = "##ctx" + std::to_string(ci);
                        if (asset_path_field("", label.c_str(), ic->context_paths[ci])) {
                            scene_->mark_dirty();
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("X")) {
                            remove_idx = ci;
                        }
                        ImGui::PopID();
                    }
                    if (remove_idx >= 0) {
                        ic->context_paths.erase(ic->context_paths.begin() + remove_idx);
                        scene_->mark_dirty();
                    }
                    if (ImGui::SmallButton("+ Context")) {
                        ic->context_paths.emplace_back();
                        scene_->mark_dirty();
                    }
                }
            }

            // Audio Source
            if (auto* au = obj->components.get<AudioSourceComponent>()) {
                colored_separator({0.55f, 0.70f, 0.85f, 1.0f});
                if (ImGui::CollapsingHeader("Audio Source", ImGuiTreeNodeFlags_DefaultOpen)) {
                    if (asset_path_field("Clip", "##AudioClip", au->clip_path)) {
                        au->sound_loaded = false;
                        scene_->mark_dirty();
                    }

                    ImGui::Text("Volume");
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::DragFloat("##AudioVolume", &au->volume, 0.01f, 0.0f, 2.0f)) scene_->mark_dirty();

                    ImGui::Text("Pitch");
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::DragFloat("##AudioPitch", &au->pitch, 0.01f, 0.1f, 4.0f)) scene_->mark_dirty();

                    ImGui::Checkbox("Loop", &au->loop);
                    ImGui::Checkbox("Spatial", &au->spatial);
                    ImGui::Checkbox("Play on Start", &au->play_on_start);

                    if (au->spatial) {
                        ImGui::Text("Min Distance");
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::DragFloat("##AudioMinDist", &au->min_distance, 0.1f, 0.1f, 100.0f)) scene_->mark_dirty();
                        ImGui::Text("Max Distance");
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::DragFloat("##AudioMaxDist", &au->max_distance, 1.0f, 1.0f, 1000.0f)) scene_->mark_dirty();
                    }

                    if (remove_button("Remove Audio Source")) {
                        obj->components.remove<AudioSourceComponent>();
                        scene_->mark_dirty();
                    }
                }
            }

            // Arrow (forward direction indicator)
            if (auto* arr_ptr = obj->components.get<ArrowComponent>()) {
                colored_separator({0.9f, 0.75f, 0.3f, 1.0f});
                if (ImGui::CollapsingHeader("Arrow", ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::Text("Length");
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::DragFloat("##ArrowLen", &arr_ptr->length, 0.1f, 0.1f, 50.0f)) scene_->mark_dirty();
                    ImGui::Text("Color");
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::ColorEdit3("##ArrowColor", &arr_ptr->color.x)) scene_->mark_dirty();
                    if (remove_button("Remove Arrow")) {
                        obj->components.remove<ArrowComponent>();
                        scene_->mark_dirty();
                    }
                }
            }

            // Add Component popup — lists types not already present
            if (ImGui::Button("Add Component", {-1, 0})) {
                ImGui::OpenPopup("##AddBuiltinComponent");
            }
            if (ImGui::BeginPopup("##AddBuiltinComponent")) {
                if (!obj->components.has<RigidbodyComponent>() && ImGui::MenuItem("Rigidbody")) {
                    obj->components.add<RigidbodyComponent>();
                    scene_->mark_dirty();
                }
                if (!obj->components.has<ColliderComponent>() && ImGui::MenuItem("Collider")) {
                    auto& col = obj->components.add<ColliderComponent>();
                    if (obj->source == "primitive:cube") {
                        col.shape = ColliderShape::Box;
                    } else if (obj->source == "primitive:sphere") {
                        col.shape = ColliderShape::Sphere;
                        col.radius = 0.5f;
                    } else {
                        col.shape = ColliderShape::Box;
                    }
                    scene_->mark_dirty();
                }
                if (!obj->components.has<EmitterComponent>() && ImGui::MenuItem("Particle Emitter")) {
                    obj->components.add<EmitterComponent>();
                    scene_->mark_dirty();
                }
                if (!obj->components.has<SpriteComponent>() && ImGui::MenuItem("Sprite")) {
                    obj->components.add<SpriteComponent>();
                    scene_->mark_dirty();
                }
                if (!obj->components.has<SpriteAnimComponent>() && ImGui::MenuItem("Sprite Animation")) {
                    obj->components.add<SpriteAnimComponent>();
                    scene_->mark_dirty();
                }
                if (!obj->components.has<TilemapComponent>() && ImGui::MenuItem("Tilemap")) {
                    auto& tm = obj->components.add<TilemapComponent>();
                    tm.tiles.resize(tm.map_size.x * tm.map_size.y, -1);
                    scene_->mark_dirty();
                }
                if (!obj->components.has<AudioSourceComponent>() && ImGui::MenuItem("Audio Source")) {
                    obj->components.add<AudioSourceComponent>();
                    scene_->mark_dirty();
                }
                if (!obj->components.has<ArrowComponent>() && ImGui::MenuItem("Arrow")) {
                    obj->components.add<ArrowComponent>();
                    scene_->mark_dirty();
                }
                if (!obj->components.has<CharacterComponent>() && ImGui::MenuItem("Character Controller")) {
                    obj->components.add<CharacterComponent>();
                    scene_->mark_dirty();
                }
                if (!obj->components.has<UiWidgetComponent>() && ImGui::MenuItem("UI Widget")) {
                    obj->components.add<UiWidgetComponent>();
                    scene_->mark_dirty();
                }
                if (!obj->components.has<InputComponent>() && ImGui::MenuItem("Input")) {
                    obj->components.add<InputComponent>().actions_dir = "assets/input/actions";
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

void PropertiesPanel::draw_material_inspector(Node3D& obj) {
    auto* mesh = obj.components.get<MeshComponent>();
    if (!mesh || !mesh->material || !pipeline_cache_ || !bindless_) return;

    auto* mat = mesh->material;
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

} // namespace fjell
