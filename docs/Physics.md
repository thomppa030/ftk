# Physics

Fjell uses [Jolt Physics](https://github.com/jrouwe/JoltPhysics) v5.2.0 for rigid body simulation.

## Lifetime

Physics only exists during play mode. `PlaySession` owns the `PhysicsWorld`:

```
start_play()
  └── PlaySession created
        └── PhysicsWorld created
              └── create_bodies(scene)    Scan objects, create Jolt bodies

each frame (play mode):
  scripts run                             Can apply forces, set velocities
  play_session_->physics_step(dt, scene)  Jolt step + transform sync

stop_play()
  └── PlaySession destroyed
        └── PhysicsWorld destroyed         All bodies removed
  └── Scene restored from snapshot
```

## Descriptors vs Live State

`RenderObject` carries **descriptors** — plain data structs that describe what kind of body to create:

```cpp
struct RigidbodyDesc {
    BodyType body_type;       // Static, Kinematic, Dynamic
    float mass;
    float friction;
    float restitution;
    float linear_damping;
    float angular_damping;
    bool gravity_enabled;
};

struct ColliderDesc {
    ColliderShape shape;      // Box, Sphere, Capsule, Mesh
    glm::vec3 half_extents;   // Box
    float radius;             // Sphere, Capsule
    float height;             // Capsule
    glm::vec3 offset;         // Local offset from object center
};
```

These are not live physics state — they're templates. When play starts, `PhysicsWorld::create_bodies()` reads them and creates corresponding Jolt bodies.

An object with a `ColliderDesc` but no `RigidbodyDesc` becomes a **static body** (e.g., a ground plane with just a collider).

## Body Types

| Type | Behavior |
|------|----------|
| **Static** | Doesn't move. Other bodies collide with it. |
| **Kinematic** | Moved by scripts (set position/rotation). Pushes dynamic bodies but isn't affected by them. |
| **Dynamic** | Moved by physics. Affected by gravity, forces, collisions. |

## Collider Shapes

| Shape | Parameters | Notes |
|-------|-----------|-------|
| **Box** | `half_extents` | Scaled by object's transform scale |
| **Sphere** | `radius` | Scaled by max(scale.x, scale.y, scale.z) |
| **Capsule** | `radius`, `height` | Radius scaled by max(scale.x, scale.z), height by scale.y |
| **Mesh** | — | Uses CPU vertex data retained during model loading |

## Character Controller

`CharacterComponent` is a capsule that walks: gravity, slopes, steps, and
collision without the toppling a dynamic body would give you. Put it on the
node instead of a rigidbody and collider for anything that walks.

### Shape

The capsule stands on the node origin by default (`feet_at_origin`), which is
where a character mesh has its feet, so a fresh component on a skeletal mesh
lands the mesh on the floor. `shape_offset` moves the capsule on top of that.
The inspector's **Fit To Mesh** sizes and places it from the mesh bounds. Node
scale applies the way it does to a capsule collider: height with y, radius
with the wider of x and z.

`step_up` is the tallest ledge walked up without a jump; `step_down` is how far
the floor may drop away before the character stops following it, which is what
keeps it on the ground going down stairs.

### Moving it

One property, `velocity`, in and out. Write the velocity you want before the
physics step; the solver writes back what it actually did.

```cpp
auto* c = self().get_component<fjell::CharacterComponent>();
c->velocity.x = input.x * speed;   // every tick, including zero when idle
c->velocity.z = input.y * speed;   // horizontal persists until rewritten
if (jump_pressed && c->ground_state == fjell::CharacterGroundState::OnGround) {
    c->velocity.y = jump_speed;    // vertical is gravity's unless you jump
}
```

While `OnGround`, vertical velocity is at least the ground's own, so a landing
stops pushing into the floor and a moving platform carries the character;
anything faster upward is a jump and survives. In the air, and on ground past
`max_slope_angle`, gravity applies, which is what makes a steep slope slide.

Writing `transform().position` teleports: the character follows the node
wherever gameplay, a parent, or the network puts it. Characters are placed in
world space, so parenting one to a moving node works.

### Crouching

Change `capsule_height` or `capsule_radius` during play and the capsule
follows at the next step. A change that would push the capsule into geometry
is refused and the component is set back to the capsule it actually has, so
after trying to stand up under a table `capsule_height` still reads the
crouched value. The same applies to a rescale.

### Collision and events

The character carries a `CollisionFilter` like a collider, on the Pawn channel
by default. Both the capsule and the sweep that moves it honour it: a collider
that ignores pawns is walked straight through, and the character's own table
can ignore a channel.

The character reports its own contacts. Walking into a wall raises
`on_collision_enter` on both the character and the wall; entering a trigger
volume raises `on_trigger_enter` on both, and leaving it `on_trigger_exit`.
Two characters bumping report each other once each way.

### Networking

`simulate` is a runtime flag. Off, physics leaves the node where something
else put it and only keeps the capsule on it, so rays and bodies still find
the character. The replication layer clears it for characters it interpolates
from another peer; the owning peer simulates.

## Transform Sync

Each physics step:

1. **Kinematic bodies:** Engine transforms are pushed into Jolt (`MoveKinematic`)
2. **Characters:** The node's world position and rotation are read through the
   parent chain; a node moved since the last step moves its character. Then
   each character sweeps its capsule with its `velocity`.
3. **Jolt step:** Forces integrated, collisions resolved
4. **Dynamic bodies:** Jolt positions/rotations pulled back to `RenderObject.transform`
5. **Characters:** World position written back through the parent chain;
   `velocity`, ground state, normal, and ground velocity updated

Jolt uses quaternions internally — so does Fjell's Transform struct, so there's no conversion overhead at the sync boundary.

## Broadphase

Two-layer broadphase:
- **NON_MOVING:** Static and kinematic bodies
- **MOVING:** Dynamic bodies

Static-vs-static collisions are skipped. Everything else is checked.

## Script API

Scripts interact with physics through `world()->physics()`:

```cpp
void tick(float dt) override {
    auto* phys = world()->physics();
    if (!phys) return; // null outside play mode

    // Apply upward impulse on space press
    if (world()->input().is_key_pressed(GLFW_KEY_SPACE)) {
        phys->apply_impulse(actor()->id(), {0.0f, 10.0f, 0.0f});
    }

    // Raycast downward to check if grounded
    auto hit = phys->raycast(
        actor()->transform().position,
        {0.0f, -1.0f, 0.0f},
        1.1f);
    if (hit.hit) {
        // grounded — hit.position, hit.normal, hit.distance, hit.object_id available
    }
}
```

Available physics methods:
- `apply_force(id, vec3)` — continuous force (call every frame)
- `apply_impulse(id, vec3)` — instantaneous velocity change
- `set_linear_velocity(id, vec3)` / `get_linear_velocity(id)`
- `set_angular_velocity(id, vec3)` / `get_angular_velocity(id)`
- `raycast(origin, direction, max_distance)` — returns `RaycastHit`

All methods are null-safe — they no-op outside play mode.

## Debug Visualization

Collider shapes are rendered as green wireframes via a separate line-list pipeline. Toggle with the "Colliders" checkbox in the viewport toolbar. Wireframes use depth testing (occluded by scene geometry) but don't write to the depth buffer (don't occlude the scene).
