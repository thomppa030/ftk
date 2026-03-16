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
| **Kinematic** | Moved by scripts (set_position/set_rotation). Pushes dynamic bodies but isn't affected by them. |
| **Dynamic** | Moved by physics. Affected by gravity, forces, collisions. |

## Collider Shapes

| Shape | Parameters | Notes |
|-------|-----------|-------|
| **Box** | `half_extents` | Scaled by object's transform scale |
| **Sphere** | `radius` | Scaled by max(scale.x, scale.y, scale.z) |
| **Capsule** | `radius`, `height` | Radius scaled by max(scale.x, scale.z), height by scale.y |
| **Mesh** | — | Uses CPU vertex data retained during model loading |

## Transform Sync

Each physics step:

1. **Kinematic bodies:** Engine transforms are pushed into Jolt (`MoveKinematic`)
2. **Jolt step:** Forces integrated, collisions resolved
3. **Dynamic bodies:** Jolt positions/rotations pulled back to `RenderObject.transform`

Jolt uses quaternions internally. Conversion happens at the sync boundary via `glm::quat` / `glm::eulerAngles`.

## Broadphase

Two-layer broadphase:
- **NON_MOVING:** Static and kinematic bodies
- **MOVING:** Dynamic bodies

Static-vs-static collisions are skipped. Everything else is checked.

## Script API

Scripts interact with physics through the `FjellEngineApi`:

```c
void (*apply_force)(uint32_t id, const float* xyz);
void (*apply_impulse)(uint32_t id, const float* xyz);
void (*set_linear_velocity)(uint32_t id, const float* xyz);
void (*get_linear_velocity)(uint32_t id, float* out_xyz);
void (*set_angular_velocity)(uint32_t id, const float* xyz);
void (*get_angular_velocity)(uint32_t id, float* out_xyz);
uint8_t (*raycast)(const float* origin, const float* direction,
                   float max_distance, float* out_hit_pos,
                   float* out_hit_normal, float* out_distance,
                   uint32_t* out_object_id);
```

All callbacks are null-safe — they check for an active `PhysicsWorld` before calling. Outside of play mode, they silently no-op.

## Debug Visualization

Collider shapes are rendered as green wireframes via a separate line-list pipeline. Toggle with the "Colliders" checkbox in the viewport toolbar. Wireframes use depth testing (occluded by scene geometry) but don't write to the depth buffer (don't occlude the scene).
