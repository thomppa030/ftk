// fjell_engine_components.hpp — Engine component types accessible from scripts
// Shipped to game projects alongside fjell_api.h.
// Layout matches engine-side structs (NodeComponent is empty, no vtable).

#pragma once

#include "fjell/api.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

extern "C" {
FJELL_SDK_EXPORT void fjell_rigidbody_add_force(const void* body, const float* force);
FJELL_SDK_EXPORT void fjell_rigidbody_add_impulse(const void* body, const float* impulse);
FJELL_SDK_EXPORT void fjell_rigidbody_linear_velocity(const void* body, float* out);
FJELL_SDK_EXPORT void fjell_rigidbody_set_linear_velocity(const void* body, const float* velocity);
FJELL_SDK_EXPORT void fjell_rigidbody_angular_velocity(const void* body, float* out);
FJELL_SDK_EXPORT void fjell_rigidbody_set_angular_velocity(const void* body, const float* velocity);
}

namespace fjell {

// ── Physics ──────────────────────────────────────────────────────────────

/// How physics treats a body.
enum class BodyType : uint8_t {
    Static,     ///< Never moves. Cheapest; for level geometry.
    Kinematic,  ///< Moved by script through its transform, not by forces.
                ///< Pushes dynamic bodies but is not pushed back.
    Dynamic,    ///< Fully simulated — gravity, collisions, forces.
};

/// Collision shape used to approximate a node for physics.
enum class ColliderShape : uint8_t {
    Box,      ///< Sized by `half_extents`.
    Sphere,   ///< Sized by `radius`.
    Capsule,  ///< Sized by `radius` and `height`; the usual character shape.
    Mesh,     ///< Exact geometry. Expensive, and static bodies only.
};

/// Makes a node participate in physics simulation. Needs a ColliderComponent
/// on the same node to have a shape to collide with.
///
/// The fields are how the body is set up; pushing it and its velocity go
/// through the methods, which reach the body in the physics world as it is
/// now. Outside play there is no physics world, and they do nothing.
///
/// @code
/// [[fj::property]] fjell::Ref<fjell::RigidbodyComponent> crate;
///
/// void fixed_tick(const fjell::FixedStep&) override {
///     crate->add_force({0.0f, thrust, 0.0f});
///     if (glm::length(crate->linear_velocity()) > 20.0f) crate->set_linear_velocity({});
/// }
/// @endcode
struct RigidbodyComponent {
    static constexpr const char* type_name = "RigidbodyComponent";

    /// Static, kinematic, or fully simulated. See `BodyType`.
    BodyType body_type{BodyType::Dynamic};

    /// Mass in kilograms. Ignored for static and kinematic bodies.
    float mass{1.0f};

    /// Surface friction, 0 (frictionless) to 1 (grippy). Contacts combine
    /// both bodies' values.
    float friction{0.5f};

    /// Bounciness, 0 (no bounce) to 1 (no energy lost).
    float restitution{0.3f};

    /// Drag on linear motion per second. Raise it to make a body settle sooner.
    float linear_damping{0.05f};

    /// Drag on rotation per second.
    float angular_damping{0.05f};

    /// Whether gravity applies. Clear it for floating or flying objects.
    bool gravity_enabled{true};

    /// How much surface the wind has to push against, in square metres. Zero
    /// leaves the body unmoved by the weather; a loose tarp wants more than
    /// a crate of the same size.
    float wind_area{0.0f};

    /// Push it through the next step, in newtons: a force acts over the one
    /// step it is added before, so a steady push is added in `fixed_tick()`,
    /// every step. For an instant change of momentum — a jump, an explosion —
    /// use `add_impulse()`.
    void add_force(const glm::vec3& force) { fjell_rigidbody_add_force(this, &force.x); }

    /// Change its momentum at once, in newton-seconds: its velocity moves by
    /// the impulse over its mass.
    void add_impulse(const glm::vec3& impulse) { fjell_rigidbody_add_impulse(this, &impulse.x); }

    /// How fast it moves, in metres per second, as the physics world has it
    /// now.
    [[nodiscard]] glm::vec3 linear_velocity() const {
        glm::vec3 velocity{0.0f};
        fjell_rigidbody_linear_velocity(this, &velocity.x);
        return velocity;
    }

    /// Move it at `velocity`, in metres per second, from now on.
    void set_linear_velocity(const glm::vec3& velocity) { fjell_rigidbody_set_linear_velocity(this, &velocity.x); }

    /// How fast it turns, in radians per second about each world axis.
    [[nodiscard]] glm::vec3 angular_velocity() const {
        glm::vec3 velocity{0.0f};
        fjell_rigidbody_angular_velocity(this, &velocity.x);
        return velocity;
    }

    /// Turn it at `velocity`, in radians per second about each world axis.
    void set_angular_velocity(const glm::vec3& velocity) { fjell_rigidbody_set_angular_velocity(this, &velocity.x); }
};

/// Category a collider belongs to, used to decide what it interacts with.
enum class CollisionChannel : uint8_t {
    world_static = 0,   ///< Level geometry that never moves.
    world_dynamic = 1,  ///< Movable props and debris.
    pawn = 2,           ///< Players and characters.
    trigger = 3,        ///< Volumes that detect without blocking.
    projectile = 4,     ///< Bullets and thrown objects.
};

/// What happens when two colliders meet.
enum class CollisionResponse : uint8_t {
    ignore = 0,   ///< Pass through; no callbacks at all.
    overlap = 1,  ///< Pass through, but fire `on_trigger_enter`/`_exit`.
    block = 2,    ///< Collide physically and fire `on_collision_enter`/`_exit`.
};

/// Which channels a collider blocks and which it merely overlaps. The masks
/// are bitfields indexed by `CollisionChannel`, so set a bit with
/// `1u << static_cast<uint32_t>(CollisionChannel::pawn)`.
///
/// `block_mask` wins where both bits are set.
struct CollisionFilter {
    /// The channel this collider belongs to — what other filters test against.
    CollisionChannel channel{CollisionChannel::world_static};

    /// Channels this collider physically blocks. Defaults to everything.
    uint32_t block_mask{0xFFFFFFFF};

    /// Channels this collider overlaps, reporting triggers without blocking.
    /// Only consulted for channels not already in `block_mask`.
    uint32_t overlap_mask{0};

    /// Resolve how this collider responds to a given channel.
    /// @param other The other collider's `channel`.
    /// @return `block` if the channel is in `block_mask`, else `overlap` if in
    ///         `overlap_mask`, else `ignore`.
    [[nodiscard]] CollisionResponse response_to(CollisionChannel other) const {
        const uint32_t bit = 1U << static_cast<uint32_t>(other);
        if ((block_mask & bit) != 0) return CollisionResponse::block;
        if ((overlap_mask & bit) != 0) return CollisionResponse::overlap;
        return CollisionResponse::ignore;
    }
};

/// Gives a node a physical shape. Pair with a RigidbodyComponent for a body
/// that moves, or use alone for static level collision.
struct ColliderComponent {
    static constexpr const char* type_name = "ColliderComponent";

    /// Take the shape from the node's imported mesh collision instead of the
    /// fields below.
    bool inherit_from_mesh{false};

    /// Which shape the size fields describe. See `ColliderShape`.
    ColliderShape shape{ColliderShape::Box};

    /// Box half-size along each axis — so a 2x2x2 box has half_extents 1.
    /// Box shape only.
    glm::vec3 half_extents{0.5f};

    /// Radius for Sphere and Capsule shapes.
    float radius{0.5f};

    /// Capsule height, measured between the cap centers rather than overall.
    float height{1.0f};

    /// Shape offset from the node's origin, in local space. Use it to stand a
    /// capsule on the node's feet rather than centering it.
    glm::vec3 offset{0.0f};

    /// Detect overlaps without blocking. Drives `on_trigger_enter`/`_exit`
    /// instead of the collision callbacks.
    bool is_trigger{false};

    /// Channel membership and what this collider blocks or overlaps.
    CollisionFilter filter{};

    /// What this collider is made of: the handle id of a `.fjsurface`, as
    /// `Asset<Surface>::handle_id()` gives it, or 0 for none. Raycast hits
    /// and contacts against the body report it back. Assign one from the
    /// inspector, or from script with an `Asset<Surface>` property; a
    /// change takes effect when the body is next built.
    uint32_t surface{0};

    /// Project-relative path of that `.fjsurface`, kept for the scene file.
    /// Engine-owned; set `surface` rather than this from script.
    std::string surface_path;
};

/// What a character is standing on.
enum class CharacterGroundState : uint8_t {
    OnGround,       ///< Supported by ground within `max_slope_angle`.
    OnSteepGround,  ///< Touching ground too steep to stand on; slides.
    NotSupported,   ///< Touching something that cannot provide support.
    InAir,          ///< Not touching ground at all.
};

/// Who moves a character. See `CharacterComponent::movement`.
enum class CharacterMovement : uint8_t {
    Intent,    ///< The engine, each fixed step, from `move_intent` and `jump()`.
    Velocity,  ///< A script, by writing `velocity` itself.
};

/// Character controller for players and NPCs — walks, climbs steps, and slides
/// off slopes without the instability of pushing a dynamic rigidbody around.
/// Use this instead of a RigidbodyComponent for anything that walks.
///
/// Say where it should go and the engine moves it, the same at any frame rate:
///
/// @code
/// void on_move(glm::vec2 stick) {
///     if (auto c = self().get_component<fjell::CharacterComponent>()) {
///         c->move_intent = glm::vec3{stick.x, 0.0f, -stick.y};
///     }
/// }
/// void on_jump() {
///     if (auto c = self().get_component<fjell::CharacterComponent>()) c->jump();
/// }
/// @endcode
struct CharacterComponent {
    static constexpr const char* type_name = "CharacterComponent";

    /// Radius of the character's capsule — roughly shoulder width.
    float capsule_radius{0.3f};

    /// Capsule height between cap centers; total standing height is this plus
    /// twice the radius.
    float capsule_height{1.4f};

    /// Steepest slope in degrees the character can stand on. Steeper ground
    /// reports `OnSteepGround` and the character slides down it.
    float max_slope_angle{50.0f};

    /// Mass in kilograms. Bounds how hard the character pushes dynamic bodies.
    float mass{80.0f};

    /// How hard the wind leans on the character, as a fraction of the force
    /// it would put on a square metre. Zero ignores the weather.
    float wind_push{0.0f};

    /// Stand the capsule on the node origin, where a character mesh has its
    /// feet. Off, the capsule is centered on the origin.
    bool feet_at_origin{true};

    /// Extra offset of the capsule center from the node origin, applied on
    /// top of `feet_at_origin`.
    glm::vec3 shape_offset{0.0f};

    /// Tallest ledge the character walks up without jumping.
    float step_up{0.4f};

    /// How far the ground may drop below the feet before the character stops
    /// following it and goes airborne. Keeps it on the floor down stairs.
    float step_down{0.5f};

    /// Channel membership and what this character blocks or overlaps. New
    /// characters are on the Pawn channel.
    CollisionFilter filter{};

    /// Whether physics moves this character. Off, the node stays where
    /// gameplay or the network puts it and the capsule follows it, so it is
    /// still hit by rays and bodies. The net layer turns this off for
    /// characters driven by another peer.
    bool simulate{true};

    /// Who moves the character. `Intent` (the default): set `move_intent` and
    /// call `jump()`, and the engine turns them into velocity every fixed step
    /// with the tuning below. `Velocity`: write `velocity` yourself, in
    /// `fixed_tick()` to replay exactly; `move_intent` and `jump()` are then
    /// ignored. A scene saved before this field existed loads as `Velocity`,
    /// which is how its scripts moved the character.
    CharacterMovement movement{CharacterMovement::Intent};

    /// Where the character wants to go, world space, with a length from 0 to
    /// 1 like a stick's deflection; only the horizontal part is used. It is
    /// cleared before input each frame, so set it from an input handler or
    /// `tick()` while there is input, and leave it when there is none: what
    /// the frame sets steers every step the frame runs. Set in `fixed_tick()`,
    /// it steers that step only.
    glm::vec3 move_intent{0.0f};

    /// Speed at full intent, in metres per second.
    float max_speed{5.0f};

    /// How fast the character reaches its intended speed on the ground, in
    /// m/s². Zero reaches it at once.
    float ground_acceleration{0.0f};

    /// How much steering the character has in the air, as a share of
    /// `air_acceleration`. With no intent in the air it keeps its momentum,
    /// so a jump commits to its arc.
    float air_control{0.35f};

    /// Steering in the air at full `air_control`, in m/s².
    float air_acceleration{12.0f};

    /// The upward speed a jump leaves the ground with, in metres per second.
    float jump_speed{5.0f};

    /// Set by `jump()` and taken by the next fixed step. Read-only.
    bool jump_requested{false};

    /// Ask for a jump. The next fixed step jumps if the character is on the
    /// ground and drops the request either way.
    void jump() { jump_requested = true; }

    /// The velocity to move with, in world units per second; the solver
    /// writes back what it actually did. Moved by `Intent`, the engine sets
    /// its horizontal part each step and a jump its vertical one. Moved by
    /// `Velocity`, write it before the physics step: horizontal components
    /// persist until rewritten, so set them every step, to zero when there
    /// is no input; vertical is gravity's, so write a positive value while
    /// `OnGround` to jump, and otherwise leave it alone.
    glm::vec3 velocity{0.0f};

    /// Whether the character is grounded. Read-only output, refreshed each
    /// fixed step — check it before allowing a jump.
    CharacterGroundState ground_state{CharacterGroundState::InAir};

    /// Surface normal underfoot; `(0,1,0)` when airborne. Read-only output,
    /// useful for aligning to slopes.
    glm::vec3 ground_normal{0.0f, 1.0f, 0.0f};

    /// Velocity of the surface underfoot, for moving platforms. Read-only output.
    glm::vec3 ground_velocity{0.0f};

    /// Surface id of the ground underfoot — the handle id of the
    /// `.fjsurface` on the collider or terrain layer being stood on, 0 when
    /// airborne or when it has none. Read-only output. Compare against the
    /// handle of an `Asset<Surface>` property to pick a footstep sound.
    uint32_t ground_surface{0};

    /// Where the capsule's center sits relative to the node origin.
    [[nodiscard]] glm::vec3 capsule_center() const {
        glm::vec3 center = shape_offset;
        if (feet_at_origin) center.y += capsule_height * 0.5f + capsule_radius;
        return center;
    }
};

// ── Rendering ────────────────────────────────────────────────────────────

/// Makes a node render a 3D model. To change shader parameters at runtime,
/// go through `material` rather than touching the fields here.
struct MeshComponent {
    static constexpr const char* type_name = "MeshComponent";

    /// Engine-owned model handle. Read for identity comparisons; assigning
    /// does not load or swap the model.
    uint32_t model_handle_id{0};

    /// Engine-owned material handle. Read-only, as above.
    uint32_t material_handle_id{0};

    /// Engine-owned model pointer. Opaque to game code.
    void* model{nullptr};

    /// Engine-owned material pointer. Opaque to game code.
    void* material{nullptr};

    /// Path of the assigned `.fjmat`, as shown in the inspector.
    std::string material_name;

    /// Engine-owned per-mesh material override, if one has been created.
    /// Opaque to game code; go through `material` to change parameters.
    void* instance{nullptr};

    /// Engine-owned material per submesh slot. Opaque to game code.
    std::vector<void*> slot_materials;
};

// ── Sprites ─────────────────────────────────────────────────────────────

/// Makes a node render a 2D sprite. Sprites are lit and depth-sorted with 3D
/// geometry, so they can sit inside a 3D scene.
struct SpriteComponent {
    static constexpr const char* type_name = "SpriteComponent";

    /// Path to the sprite's texture, relative to the project root.
    std::string texture_path;

    /// RGBA multiplied into the texture. White leaves it unchanged; use it to
    /// flash or team-color a sprite.
    glm::vec4 tint{1.0f, 1.0f, 1.0f, 1.0f};

    /// Sub-rectangle of the texture to draw, as normalized `(u_min, v_min,
    /// u_max, v_max)`. Defaults to the whole image; a SpriteAnimComponent
    /// drives this for sheet animation.
    glm::vec4 uv_rect{0.0f, 0.0f, 1.0f, 1.0f};

    /// Overall alpha, 0 transparent to 1 opaque. Multiplies with `tint.a`.
    float opacity{1.0f};

    /// Draw only in the editor viewport, never in play. For gizmos and markers.
    bool editor_only{false};

    /// Mirror horizontally. Cheaper than authoring a flipped texture for
    /// left/right facing.
    bool flip_h{false};

    /// Lie flat in the node's XY plane instead of facing the camera. For
    /// floor decals and top-down games.
    bool flat{false};

    /// Use nearest-neighbour filtering, keeping hard pixel edges instead of
    /// blurring between texels.
    bool pixel_art{false};

    /// Texture pixels per world unit, so the sprite sizes itself from the
    /// texture. Zero means the node's scale sets the size.
    float pixels_per_unit{0.0f};

    /// Explicit draw layer; lower draws behind. Sprites in different layers
    /// never interleave regardless of depth.
    int32_t sort_layer{0};

    /// Draw order within the same `sort_layer`; higher draws on top.
    int32_t sort_order{0};

    /// Take scene lighting instead of drawing unlit.
    bool lit{false};

    /// Optional normal map for a lit sprite; empty derives normals from the
    /// quad plane.
    std::string normal_path;

    /// Surface roughness for a lit sprite, 0 mirror to 1 matte.
    float roughness{1.0f};

    /// Metalness for a lit sprite, 0 dielectric to 1 metal.
    float metallic{0.0f};

    /// Emission strength, a multiplier on `tint` that also drives bloom.
    float emission{0.0f};

    /// Engine-owned bindless texture index, assigned once the texture loads.
    uint32_t texture_index{0};

    /// Engine-owned bindless index of the normal map, once it loads.
    uint32_t normal_index{0};

    /// Source texture width in pixels. Valid once `texture_loaded` is true.
    uint32_t tex_width{0};

    /// Source texture height in pixels. Valid once `texture_loaded` is true.
    uint32_t tex_height{0};

    /// Whether the texture has finished loading. The size fields are 0 until
    /// it does.
    bool texture_loaded{false};

    /// Whether the normal map has finished loading.
    bool normal_loaded{false};
};

// Every mirrored struct and its size as this header defines it, published by
// FJELL_REGISTER() so the engine can check the module was built against the
// layouts it has. A struct added here must be added to this list.
inline constexpr FjellComponentLayout ENGINE_COMPONENT_LAYOUTS[] = {
    {RigidbodyComponent::type_name, sizeof(RigidbodyComponent)},
    {ColliderComponent::type_name, sizeof(ColliderComponent)},
    {CharacterComponent::type_name, sizeof(CharacterComponent)},
    {MeshComponent::type_name, sizeof(MeshComponent)},
    {SpriteComponent::type_name, sizeof(SpriteComponent)},
};

} // namespace fjell
