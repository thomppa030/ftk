# Physics

Fjell uses [Jolt Physics](https://github.com/jrouwe/JoltPhysics) v5.2.0 for rigid body simulation.

## Lifetime

Physics only exists during play mode. `PhysicsSubsystem` owns the
`PhysicsWorld`:

```
play starts
  └── PhysicsWorld created
        └── create_bodies(scene)          A body for every node that needs one

each fixed step (0, 1 or several a frame)
  └── PhysicsSubsystem::fixed_tick()      Wind, buoyancy, then PhysicsWorld::step()

play stops
  └── PhysicsWorld destroyed              Every body goes with it
  └── Scene restored from the snapshot
```

Physics steps with the rest of the simulation, at the project's fixed tick
rate (60 Hz unless Project Settings > Simulation says otherwise), never with a
frame's delta. See [Main Loop](Main-Loop.md#the-fixed-step).

## Components are templates

`RigidbodyComponent` (body type, mass, friction, restitution, damping, gravity,
wind area) and `ColliderComponent` (shape, extents, offset, collision filter,
surface) describe the body to make; they are not live physics state. When play
starts, `PhysicsWorld::create_bodies()` reads them and creates the Jolt bodies.

A node with a collider and no rigidbody becomes a **static body** — a floor, a
wall.

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

Who moves a character is its `movement`:

- **`Intent`** (new characters): say where it should go and the engine moves
  it. Set `move_intent` — a world direction, length 0 to 1 like a stick's
  deflection — and call `jump()` to jump. Every fixed step the engine turns
  them into velocity with the character's tuning: `max_speed`,
  `ground_acceleration` (0 reaches full speed at once), `air_control` and
  `air_acceleration` for steering in the air, `jump_speed`. With no intent on
  the ground it stops; in the air it keeps its momentum, so a jump commits to
  its arc.
- **`Velocity`**: write `velocity` yourself and the solver writes back what it
  actually did. `move_intent` and `jump()` are ignored. Scenes saved before
  `movement` existed load as `Velocity`, which is how their scripts moved.

```cpp
// Intent, from input handlers bound with InputAPI::bind: the frame's input
// steers every step of the frame. camera_relative() is the game's own: the
// stick turned into a world direction.
void on_move(glm::vec2 value) {
    auto* c = self().get_component<fjell::CharacterComponent>();
    c->move_intent = camera_relative(value);
}
void on_jump() {
    auto* c = self().get_component<fjell::CharacterComponent>();
    if (c->ground_state == fjell::CharacterGroundState::OnGround) c->jump();
}
```

**Intent is per frame, or per step.** `move_intent` is cleared before input
each frame, so a held stick sets it again every frame and a released one
leaves it zero, with no script resetting it. What the frame set — from input
handlers and `tick()` — steers every step that frame runs. Set in
`fixed_tick()`, it steers that step only: the next step starts again from the
frame's intent. A jump asked for is taken by the next step, which jumps if the
character is on the ground and drops the request either way.

**Velocity belongs in `fixed_tick()`.** A character moved by `Velocity` and
written in `tick()` moves by a frame's decision on every step, which differs
between frame rates; the editor reports a script that does so. Horizontal
velocity persists until rewritten, so write it every step, to zero when idle;
vertical is gravity's, so write a positive value while `OnGround` to jump and
otherwise leave it alone. A script that adds a `CharacterComponent` at runtime
and writes its velocity has to set `movement = Velocity` first, since new
characters are `Intent`.

While `OnGround`, vertical velocity is at least the ground's own, so a landing
stops pushing into the floor and a lift carries the character up and down;
anything faster upward is a jump and survives. A platform moving sideways does
not carry the character yet: only the ground's vertical velocity is taken. In
the air, and on ground past `max_slope_angle`, gravity applies, which is what
makes a steep slope slide.

Writing `transform().position` teleports: the character follows the node
wherever gameplay, a parent, or the network puts it. Characters are placed in
world space, so parenting one to a moving node works. `Node3D::teleport()`
does the same and is also drawn there at once rather than blended there.

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

## Each fixed step

`PhysicsSubsystem::fixed_tick()` runs once per step, after the scripts'
`fixed_tick()`:

1. **Forces.** Wind on every dynamic body with a `wind_area` and every
   character with `wind_push`, and buoyancy on every body floating in water, sampled at the
   step's time. Jolt clears accumulated forces after every step, so a force is
   applied in each step it should act in.
2. **Kinematic bodies and bone bodies** move toward where the scene puts them.
   A kinematic node moved inside the step (by a `fixed_tick()`) reaches its
   target that step. One moved once a frame (in `tick()`, or by an animation)
   covers an even share of the way on each of the frame's remaining steps, so a
   platform moved per frame at 30 fps moves on both of the frame's steps
   rather than jumping on the first and stopping on the second.
3. **Characters.** The node's world placement is read through the parent chain;
   a node moved since the last step moves its character. An `Intent` character
   is steered, then each character sweeps its capsule with its `velocity`.
4. **Jolt step.** Forces integrated, collisions resolved, in one collision pass
   per sixtieth of a second (`collision_steps_for()`), so a slower tick rate
   takes more passes rather than letting fast bodies pass through thin ones.
5. **Contact events** are sorted: Jolt reports them from its worker threads in
   whatever order they finish, and gameplay answering them must not depend on
   that. They are dispatched to scripts after the step, before the next one.
6. **Read back.** Dynamic bodies' placements into their nodes; characters'
   positions through the parent chain, with `velocity`, ground state, normal,
   ground velocity and ground surface.
7. **Intent.** Each character's `move_intent` goes back to what the frame set,
   for the next step.

Jolt uses quaternions internally — so does Fjell's Transform struct, so there's no conversion overhead at the sync boundary.

## Broadphase

Two-layer broadphase:
- **NON_MOVING:** Static and kinematic bodies
- **MOVING:** Dynamic bodies

Static-vs-static collisions are skipped. Everything else is checked.

## Script API

Scripts reach the running world through `fjell::PhysicsAPI`
(`fjell/physics_api.hpp`). Every call is inert outside play mode.

```cpp
void fixed_tick(const fjell::FixedStep& step) override {
    // A continuous push acts on the step it is applied in, so apply it in
    // every step it should act.
    fjell::PhysicsAPI::apply_force(id(), thrust_);

    fjell::PhysicsHit hit;
    if (fjell::PhysicsAPI::raycast(transform().position, {0.0f, -1.0f, 0.0f}, 1.1f, hit,
                                   {FJ_MASK_ALL, id()})) {
        // grounded: hit.position, hit.normal, hit.distance, hit.node_id, hit.surface
    }
}
```

- `apply_force(id, vec3)` — a push over the step. Jolt clears forces after
  every step, so apply it in `fixed_tick()`: from `tick()` it only reaches the
  first step of the frame.
- `apply_impulse(id, vec3)` — an instant change of momentum.
- `set_linear_velocity` / `linear_velocity`, `set_angular_velocity` /
  `angular_velocity` — the body's motion as the last step left it.
- `raycast`, `raycast_all`, `sphere_cast`, `box_cast`, `capsule_cast`,
  `overlap_sphere`, `overlap_box` — queries against the world as the last step
  left it, narrowed by a `PhysicsQuery` (channels, a node to ignore).
- `set_gravity` / `gravity`.

**Don't write a dynamic body's transform.** Physics owns where a dynamic body
is, and puts it back at its next step; the editor reports a script that moves
one from `tick()` or `late_tick()`. Push it with forces or its velocity, or
move it with `Node3D::teleport()`.

## Debug Visualization

Collider shapes are rendered as green wireframes via a separate line-list pipeline. Toggle with the "Colliders" checkbox in the viewport toolbar. Wireframes use depth testing (occluded by scene geometry) but don't write to the depth buffer (don't occlude the scene).
