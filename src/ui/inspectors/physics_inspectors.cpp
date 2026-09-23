#include "ui/inspectors/physics_inspectors.hpp"

#include "core/asset_type_registry.hpp"
#include "core/project_manager.hpp"
#include "physics/buoyancy.hpp"
#include "physics/collider_surface.hpp"
#include "physics/physics_types.hpp"
#include "renderer/components/mesh_component.hpp"
#include "renderer/components/skeletal_mesh_component.hpp"
#include "renderer/frustum.hpp"
#include "renderer/resources/model.hpp"
#include "renderer/resources/skeletal_model.hpp"
#include "scene/component_meta.hpp"
#include "scene/node.hpp"
#include "ui/asset_ref_widget.hpp"
#include "ui/inspectors/collision_filter_inspector.hpp"
#include "ui/kit/button.hpp"
#include "ui/kit/choice.hpp"
#include "ui/kit/feedback.hpp"
#include "ui/kit/field.hpp"
#include "ui/kit/icons.hpp"
#include "ui/kit/list_editor.hpp"
#include "ui/kit/row.hpp"
#include "ui/kit/section.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace fjell {

namespace {

constexpr ui::Choice<BodyType> BODY_TYPES[] = {
    {BodyType::Static, "Static"},
    {BodyType::Kinematic, "Kinematic"},
    {BodyType::Dynamic, "Dynamic"},
};

constexpr ui::Choice<ColliderShape> SHAPES[] = {
    {ColliderShape::Box, "Box"},
    {ColliderShape::Sphere, "Sphere"},
    {ColliderShape::Capsule, "Capsule"},
    {ColliderShape::Mesh, "Mesh"},
};

const char* ground_state_name(CharacterGroundState state) {
    switch (state) {
        case CharacterGroundState::OnGround:      return "On ground";
        case CharacterGroundState::OnSteepGround: return "On steep ground";
        case CharacterGroundState::NotSupported:  return "Not supported";
        case CharacterGroundState::InAir:         return "In air";
    }
    return "Unknown";
}

// The local bounds of whatever the node renders, if anything.
bool mesh_bounds(const Node3D& node, AABB& out) {
    if (const auto* skeletal = node.components.get<SkeletalMeshComponent>();
        skeletal != nullptr && skeletal->skeletal_model != nullptr) {
        out = skeletal->skeletal_model->local_aabb();
        return out.min.x <= out.max.x;
    }
    if (const auto* mesh = node.components.get<MeshComponent>();
        mesh != nullptr && mesh->model != nullptr) {
        out = mesh->model->local_aabb();
        return out.min.x <= out.max.x;
    }
    return false;
}

// Sizes the capsule to the mesh: as tall as the bounds, as wide as their
// wider horizontal side, and standing on the bounds' floor rather than on
// the origin when the mesh does not start there.
void fit_to_bounds(CharacterComponent& cc, const AABB& bounds) {
    const glm::vec3 size = bounds.max - bounds.min;
    float radius = std::max(size.x, size.z) * 0.5f;
    // A capsule is at least two radii tall, so a wide, short mesh gets a
    // radius that still fits its height.
    radius = std::min(radius, size.y * 0.5f);
    cc.capsule_radius = std::max(radius, 0.01f);
    cc.capsule_height = std::max(size.y - 2.0f * cc.capsule_radius, 0.0f);
    cc.feet_at_origin = true;
    const glm::vec3 center = (bounds.min + bounds.max) * 0.5f;
    cc.shape_offset = {center.x, bounds.min.y, center.z};
}

// A button for an action, led by its icon.
bool action(const char* icon, const char* label, ui::ButtonKind kind = ui::ButtonKind::Secondary) {
    const std::string text = std::string(icon) + "  " + label;
    return ui::button(text.c_str(), kind);
}

ui::Edit surface_row(ColliderComponent& col, const InspectorContext& ctx) {
    ui::Edit edit;
    static const std::string surface_exts[] = {".fjsurface"};
    const auto* surface_type = AssetTypeRegistry::instance().find_by_name("SurfaceAsset");
    ui::row("Surface", "What the collider is made of, for footsteps, impacts and decals.", [&] {
        if (asset_slot({.label = "Surface", .id = "##surface", .extensions = surface_exts,
                        .ctx = &ctx, .asset_type = surface_type},
                       col.surface_path)) {
            load_collider_surface(col, ctx.resource_manager,
                                  ctx.project_mgr != nullptr ? ctx.project_mgr->active() : nullptr);
            edit = {true, true};
        }
    });
    return edit;
}

} // namespace

ui::Edit draw_rigidbody(Node3D& /*node*/, void* component, const InspectorContext& /*ctx*/) {
    auto& rb = *static_cast<RigidbodyComponent*>(component);
    ui::Edit edit;
    if (auto t = ui::PropertyTable("##rigidbody")) {
        ui::row("Body type", [&] { edit |= ui::choice("##body_type", rb.body_type, BODY_TYPES); });
        ui::row("Mass", [&] {
            edit |= ui::drag("##mass", rb.mass,
                             {.speed = 0.1f, .min = 0.01f, .max = 1000.0f, .unit = ui::Unit::Kilograms});
        });
        ui::row("Friction", [&] {
            edit |= ui::drag("##friction", rb.friction, {.speed = 0.01f, .min = 0.0f, .max = 2.0f});
        });
        ui::row("Restitution", "How much of its speed a body keeps when it bounces: 0 stops dead, 1 bounces back as fast.",
                [&] {
                    edit |= ui::drag("##restitution", rb.restitution,
                                     {.speed = 0.01f, .min = 0.0f, .max = 2.0f});
                });
        ui::row("Linear damping", [&] {
            edit |= ui::drag("##linear_damping", rb.linear_damping,
                             {.speed = 0.01f, .min = 0.0f, .max = 10.0f});
        });
        ui::row("Angular damping", [&] {
            edit |= ui::drag("##angular_damping", rb.angular_damping,
                             {.speed = 0.01f, .min = 0.0f, .max = 10.0f});
        });
        ui::row("Gravity", [&] { edit |= ui::checkbox("##gravity", rb.gravity_enabled); });
        ui::row("Wind area", "Surface the weather's wind pushes against. 0 leaves the body unmoved by it.",
                [&] {
                    edit |= ui::drag("##wind_area", rb.wind_area,
                                     {.speed = 0.05f, .min = 0.0f, .max = 100.0f,
                                      .unit = ui::Unit::SquareMetres});
                });
    }
    return edit;
}

ui::Edit draw_collider(Node3D& node, void* component, const InspectorContext& ctx) {
    auto& col = *static_cast<ColliderComponent*>(component);
    ui::Edit edit;
    const bool has_mesh = node.components.has<MeshComponent>();
    const bool inherited = col.inherit_from_mesh && has_mesh;
    if (auto t = ui::PropertyTable("##collider")) {
        ui::row("Inherit from mesh", "Use the collision authored on the mesh asset, set up once for every object placed from it.",
                [&] {
                    ImGui::BeginDisabled(!has_mesh);
                    edit |= ui::checkbox("##inherit", col.inherit_from_mesh);
                    ImGui::EndDisabled();
                    if (!has_mesh) ImGui::SetItemTooltip("Needs a Mesh component");
                });
        if (inherited) {
            ui::hint("Collision comes from the mesh asset");
        } else {
            ui::row("Shape", [&] { edit |= ui::choice("##shape", col.shape, SHAPES); });
            switch (col.shape) {
                case ColliderShape::Box:
                    ui::row("Half extents", [&] {
                        edit |= ui::vec3("##half_extents", col.half_extents,
                                         {.speed = 0.05f, .min = 0.01f, .max = 100.0f,
                                          .unit = ui::Unit::Metres});
                    });
                    break;
                case ColliderShape::Sphere:
                    ui::row("Radius", [&] {
                        edit |= ui::drag("##radius", col.radius, {.speed = 0.05f, .min = 0.01f,
                                                                  .max = 100.0f, .unit = ui::Unit::Metres});
                    });
                    break;
                case ColliderShape::Capsule:
                    ui::row("Radius", [&] {
                        edit |= ui::drag("##radius", col.radius, {.speed = 0.05f, .min = 0.01f,
                                                                  .max = 100.0f, .unit = ui::Unit::Metres});
                    });
                    ui::row("Height", [&] {
                        edit |= ui::drag("##height", col.height, {.speed = 0.05f, .min = 0.01f,
                                                                  .max = 100.0f, .unit = ui::Unit::Metres});
                    });
                    break;
                case ColliderShape::Mesh:
                    ui::hint("Uses the model's collision mesh");
                    break;
            }
            ui::row("Offset", [&] {
                edit |= ui::vec3("##offset", col.offset, {.speed = 0.05f, .unit = ui::Unit::Metres});
            });
        }
        ui::row("Trigger", "Reports what enters and leaves it instead of blocking.",
                [&] { edit |= ui::checkbox("##trigger", col.is_trigger); });
        edit |= surface_row(col, ctx);
    }
    edit |= draw_collision_filter(col.filter);
    return edit;
}

ui::Edit draw_character(Node3D& node, void* component, const InspectorContext& /*ctx*/) {
    auto& cc = *static_cast<CharacterComponent*>(component);
    ui::Edit edit;
    AABB bounds;
    const bool has_bounds = mesh_bounds(node, bounds);
    if (auto t = ui::PropertyTable("##character")) {
        ui::row("Capsule radius", [&] {
            edit |= ui::drag("##radius", cc.capsule_radius,
                             {.speed = 0.01f, .min = 0.01f, .max = 10.0f, .unit = ui::Unit::Metres});
        });
        ui::row("Capsule height", [&] {
            edit |= ui::drag("##height", cc.capsule_height,
                             {.speed = 0.01f, .min = 0.01f, .max = 20.0f, .unit = ui::Unit::Metres});
        });
        ui::row("", [&] {
            ImGui::BeginDisabled(!has_bounds);
            if (action(ui::icon::use_selected, "Fit to mesh")) {
                fit_to_bounds(cc, bounds);
                edit = {true, true};
            }
            ImGui::EndDisabled();
            ImGui::SetItemTooltip(has_bounds ? "Size and place the capsule from the mesh bounds"
                                             : "Needs a mesh or skeletal mesh on this node");
        });
        ui::row("Feet at origin", "Stand the capsule on the node origin, where a character mesh has its feet.",
                [&] { edit |= ui::checkbox("##feet", cc.feet_at_origin); });
        ui::row("Shape offset", [&] {
            edit |= ui::vec3("##offset", cc.shape_offset, {.speed = 0.01f, .unit = ui::Unit::Metres});
        });
        ui::row("Max slope", [&] {
            edit |= ui::slider("##slope", cc.max_slope_angle, 0.0f, 89.0f, ui::Unit::Degrees, "%.1f");
        });
        ui::row("Step up", [&] {
            edit |= ui::drag("##step_up", cc.step_up,
                             {.speed = 0.01f, .min = 0.0f, .max = 5.0f, .unit = ui::Unit::Metres});
        });
        ui::row("Step down", [&] {
            edit |= ui::drag("##step_down", cc.step_down,
                             {.speed = 0.01f, .min = 0.0f, .max = 5.0f, .unit = ui::Unit::Metres});
        });
        ui::row("Mass", [&] {
            edit |= ui::drag("##mass", cc.mass,
                             {.speed = 0.5f, .min = 1.0f, .max = 500.0f, .unit = ui::Unit::Kilograms});
        });
        ui::row("Wind push", "How hard the weather's wind leans on the character. 0 ignores it.", [&] {
            edit |= ui::drag("##wind", cc.wind_push, {.speed = 0.05f, .min = 0.0f, .max = 20.0f});
        });
    }
    edit |= draw_collision_filter(cc.filter);

    ui::subheading("State");
    if (auto t = ui::PropertyTable("##state")) {
        ui::row("Ground", [&] { ui::readout(ground_state_name(cc.ground_state)); });
        ui::row("Velocity", [&] {
            char text[64];
            std::snprintf(text, sizeof(text), "%.2f, %.2f, %.2f m/s", cc.velocity.x, cc.velocity.y,
                          cc.velocity.z);
            ui::readout(text);
        });
        if (!cc.simulate) ui::hint("Not simulated: driven from outside physics");
    }
    return edit;
}

ui::Edit draw_buoyancy(Node3D& node, void* component, const InspectorContext& /*ctx*/) {
    auto& b = *static_cast<BuoyancyComponent*>(component);
    ui::Edit edit;

    // Buoyancy is a force on a body, so without one there is nothing to
    // push. Said here rather than left to be discovered by a crate that
    // refuses to float.
    const auto* rb = node.components.get<RigidbodyComponent>();
    const auto* col = node.components.get<ColliderComponent>();
    if (rb == nullptr) {
        ui::status(ui::Severity::Warning, "Needs a Rigidbody to float");
    } else if (rb->body_type != BodyType::Dynamic) {
        ui::status(ui::Severity::Warning, "Only a Dynamic rigidbody floats");
    }
    if (col == nullptr) {
        ui::status(ui::Severity::Warning, "Needs a Collider for its size");
    }

    if (auto t = ui::PropertyTable("##buoyancy")) {
        ui::row("Density", "How heavy this is for its size, against water. Below 1 floats, 1 hangs, "
                           "above sinks. Oak is about 0.75, a sealed drum nearer 0.2.",
                [&] {
                    edit |= ui::drag("##density", b.density, {.speed = 0.01f, .min = 0.0f, .max = 5.0f});
                });
        // The rigidbody's Mass is driven by this while buoyancy is on, so
        // say what it comes to: two numbers for one property is what made a
        // cube at density 0.1 sink.
        if (col != nullptr) {
            const BuoyancySamples s = buoyancy_samples(b, col);
            if (s.volume > 0.0f) {
                char weight[96];
                std::snprintf(weight, sizeof(weight),
                              "%.3g m\xc2\xb3 weighs %.3g kg at this density, the Rigidbody's mass",
                              static_cast<double>(s.volume),
                              static_cast<double>(s.volume * b.density * WATER_DENSITY));
                ui::hint(weight);
                char submerged[48];
                std::snprintf(submerged, sizeof(submerged), "Floats %.0f %% under water",
                              static_cast<double>(std::min(b.density, 1.0f) * 100.0f));
                ui::hint(submerged);
            }
        }
        ui::row("Drag", "How hard the water resists it moving. This is what settles a float instead "
                        "of letting it bob forever.",
                [&] { edit |= ui::drag("##drag", b.drag, {.speed = 0.05f, .min = 0.0f, .max = 20.0f}); });
        ui::row("Angular drag", "How hard the water resists it turning. Separate from Drag so a hull "
                                "can glide forward and still resist being spun.",
                [&] {
                    edit |= ui::drag("##angular_drag", b.angular_drag,
                                     {.speed = 0.05f, .min = 0.0f, .max = 20.0f});
                });
        ui::row("Follow current", "Carried along by the water it sits in, so a raft drifts with a swell "
                                  "instead of staying put under it.",
                [&] { edit |= ui::checkbox("##follow", b.follow_current); });
    }

    ui::subheading("Float points");
    edit |= ui::list_editor("##points", b.points, "Add point",
                            "Floating on the collider's shape: a box on its corners, a round "
                            "shape on its axes. Add points only for a hull whose float points "
                            "are not its shape",
                            [](glm::vec3& p, std::size_t) {
                                return ui::vec3("##point", p, {.speed = 0.05f, .unit = ui::Unit::Metres});
                            });
    // Back to floating on the collider's shape, beside the list's Add.
    if (!b.points.empty()) {
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        if (action(ui::icon::remove, "Clear points", ui::ButtonKind::GhostDanger)) {
            b.points.clear();
            edit = {true, true};
        }
        ImGui::SetItemTooltip("Float on the collider's shape again");
    }
    return edit;
}

} // namespace fjell
