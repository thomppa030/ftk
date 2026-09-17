#pragma once

#include "physics/collision_channel.hpp"
#include "scene/node_component.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace fjell {

enum class BodyType : uint8_t { Static, Kinematic, Dynamic };
enum class ColliderShape : uint8_t { Box, Sphere, Capsule, Mesh };

struct RigidbodyComponent : NodeComponent {
    BodyType body_type{BodyType::Dynamic};
    float mass{1.0f};
    float friction{0.5f};
    float restitution{0.3f};
    float linear_damping{0.05f};
    float angular_damping{0.05f};
    bool gravity_enabled{true};
    // Surface the wind pushes against, in square metres; 0 ignores it.
    float wind_area{0.0f};
};

/// What makes a body float, and how it sits when it does.
///
/// Buoyancy is sampled at several points rather than one, because a single
/// force at the centre of mass can only heave a body up and down. A hull
/// lifted harder at the bow than at the stern pitches, and that difference —
/// several forces disagreeing across one body — is the whole of why a boat
/// rolls in a swell instead of bobbing like a cork.
///
/// The points come from the node's collider unless a list is authored here,
/// so a crate floats correctly with one tick and no placement, and stays
/// right when its collider is resized.
struct BuoyancyComponent : NodeComponent {
    /// How heavy this body is for its size, as a fraction of water. Below 1
    /// it floats, at 1 it hangs wherever it is put, above it sinks.
    ///
    /// Density rather than a volume because it is the number with a knowable
    /// answer: oak is about 0.75, a sealed steel drum nearer 0.2, a body in
    /// water a little under 1. The volume the force is computed from comes
    /// from the collider, which already knows the size.
    float density{0.6f};

    /// How hard the water resists a body moving through it, per sample point
    /// under the surface. This is what stops a float oscillating forever:
    /// without it a body dropped in water bounces on its own buoyancy, since
    /// nothing takes the energy back out.
    float drag{1.2f};

    /// How hard the water resists a body turning. Held separate from `drag`
    /// because a hull that should glide forward still ought to resist being
    /// spun, and one number cannot say both.
    float angular_drag{0.8f};

    /// Sample points in the node's local space, overriding the ones derived
    /// from the collider. Empty is the normal case.
    ///
    /// For a hull whose float points are not its corners: a keel that should
    /// sit deep, a bow that should lift early. Authored points are used
    /// exactly as given, so they are also the way to make a body list or
    /// float bow-high on purpose.
    std::vector<glm::vec3> points;

    /// Whether the body is pushed along by the water it sits in, so a raft
    /// drifts with a swell instead of staying put under it.
    bool follow_current{true};
};

struct ColliderComponent : NodeComponent {
    /// Use the collision setup authored on the node's mesh instead of the
    /// fields below. Set collision up once on the asset rather than on every
    /// object placed from it. Ignored when the node has no mesh.
    bool inherit_from_mesh{false};

    ColliderShape shape{ColliderShape::Box};
    glm::vec3 half_extents{0.5f};
    float radius{0.5f};
    float height{1.0f};
    glm::vec3 offset{0.0f};
    bool is_trigger{false};

    // What this collider is, and what it does about everything else.
    CollisionFilter filter{default_filter_for(CollisionChannel::world_static)};

    // The surface this collider is made of, as the SurfaceAssetHandle id of a
    // .fjsurface (0 for none) and the project-relative path it was loaded
    // from. Queries and contacts against the body report the id. Mirrored by
    // the script SDK's copy of this struct.
    uint32_t surface{0};
    std::string surface_path;
};

// Shapes a collider to match the primitive mesh a node was built from. The
// primitives are unit-sized, so this is exact rather than a guess: a cube is
// 1m across, a sphere 1m in diameter, a plane 1m square with no thickness.
// Returns false when the source is not a primitive and the caller should leave
// the collider alone.
bool fit_collider_to_primitive(ColliderComponent& collider, const std::string& source);

struct Node;

// The shape a collider actually collides as: Mesh when it inherits from a
// mesh that carries collision, its own `shape` otherwise. Physics builds the
// body from this and overlays draw it, so the two never disagree.
[[nodiscard]] ColliderShape effective_collider_shape(const ColliderComponent& collider,
                                                     const Node& node);

enum class CharacterGroundState : uint8_t { OnGround, OnSteepGround, NotSupported, InAir };

// A capsule that walks. Field order is mirrored by the script SDK's copy of
// this struct; change both together.
struct CharacterComponent : NodeComponent {
    float capsule_radius{0.3f};
    float capsule_height{1.4f};
    float max_slope_angle{50.0f};
    float mass{80.0f};

    // The node origin is where a character mesh has its feet, so the capsule
    // is lifted to stand on it. Off, the capsule is centered on the origin.
    // How hard the wind leans on the character, per square metre; 0 ignores it.
    float wind_push{0.0f};
    bool feet_at_origin{true};
    // Applied on top of that, for a capsule that sits off-center by design.
    glm::vec3 shape_offset{0.0f};

    // Tallest ledge walked up without jumping, and how far below the feet the
    // ground may drop before the character stops following it.
    float step_up{0.4f};
    float step_down{0.5f};

    // What the character is to collision and what it collides with. The
    // capsule and the sweep that moves it both honour this, so a collider
    // that ignores pawns is walked straight through.
    CollisionFilter filter{default_filter_for(CollisionChannel::pawn)};

    // Off, physics leaves the node where something else put it and only keeps
    // the capsule there so rays and bodies still find the character. That is
    // what a networked character driven by another peer needs. Runtime only.
    bool simulate{true};

    // The velocity to move with, written by gameplay before the step, and the
    // velocity the solver ended up with, written back after it. Horizontal
    // components persist until rewritten; vertical is gravity's unless a
    // positive value is written while grounded, which is a jump.
    glm::vec3 velocity{0.0f};

    // Runtime (not serialized, updated by physics each frame)
    CharacterGroundState ground_state{CharacterGroundState::InAir};
    glm::vec3 ground_normal{0.0f, 1.0f, 0.0f};
    glm::vec3 ground_velocity{0.0f};
    // Surface id of the ground underfoot, 0 when airborne or on a collider
    // with no surface. What a footstep reads.
    uint32_t ground_surface{0};

    // Where the capsule's center sits relative to the node origin.
    [[nodiscard]] glm::vec3 capsule_center() const {
        glm::vec3 center = shape_offset;
        if (feet_at_origin) center.y += capsule_height * 0.5f + capsule_radius;
        return center;
    }
};

// The capsule a character collides as once the node's scale is applied, the
// same way a capsule collider scales: height with y, radius with the wider of
// x and z. Everything that builds or draws the capsule goes through this so
// the two cannot disagree.
struct CharacterCapsule {
    float radius;
    float height;
    glm::vec3 center;
};

[[nodiscard]] inline CharacterCapsule scaled_capsule(const CharacterComponent& character,
                                                     const glm::vec3& scale) {
    CharacterCapsule capsule;
    capsule.radius = character.capsule_radius * std::max(scale.x, scale.z);
    capsule.height = character.capsule_height * scale.y;
    capsule.center = character.shape_offset * scale;
    if (character.feet_at_origin) capsule.center.y += capsule.height * 0.5f + capsule.radius;
    return capsule;
}

struct CollisionEvent {
    uint32_t other_id{0};
    glm::vec3 contact_point{0.0f};
    glm::vec3 contact_normal{0.0f};
    // Surface id of the other body where it was touched, 0 for none.
    uint32_t surface{0};
};

struct RaycastHit {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f};
    float distance{0.0f};
    uint32_t object_id{0};
    // Surface id of the shape that was hit, 0 for none.
    uint32_t surface{0};
};

// Which bodies a query considers. Expressed in the terms gameplay reasons
// about — channels and nodes — so a backend swap rewrites the collector and
// leaves every call site alone.
struct QueryFilter {
    // Bit N set means channel N is eligible. Defaults to everything.
    uint32_t channel_mask{0xFFFFFFFF};

    // Nodes to skip. The common case is one entry: whatever fired the query,
    // which would otherwise hit itself from the inside.
    std::vector<uint32_t> ignore_nodes;

    [[nodiscard]] bool allows(CollisionChannel channel) const {
        return (channel_mask & (1U << static_cast<uint32_t>(channel))) != 0;
    }

    [[nodiscard]] bool ignores(uint32_t node_id) const {
        return std::find(ignore_nodes.begin(), ignore_nodes.end(), node_id) !=
               ignore_nodes.end();
    }
};

// The shape a cast sweeps. A ray is the degenerate case and has its own entry
// point, since it is by far the most common and needs no shape at all.
enum class QueryShape : uint8_t { sphere, box, capsule };

} // namespace fjell
