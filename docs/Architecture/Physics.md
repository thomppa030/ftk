# Physics

> Partly written. The simulation step below is current; the sections listed
> at the end are still to write. For using physics from a game, see
> [Physics](../Physics.md).

## Scope
Jolt Physics integration: world setup, body creation, scene sync, queries.

## The simulation step

Physics is one part of the simulation, and the simulation advances in fixed
steps: every step the same length (1/60 s by default, the project's
`simulation.fixed_tick_rate`), as many per frame as the frame's game time
covers. Nothing in it reads a frame's delta. That is what makes a game behave
the same at 30 fps and at 144, and what re-simulation for netcode will need:
replaying the same inputs over the same steps gives the same world.

The pieces, and where they live:

| Piece | Where | What it does |
|---|---|---|
| `FixedTimestep` | `core/fixed_timestep.hpp` | Turns frame time into whole steps, carries the rest, gives each step its `dt`, `time` (counted as `index × dt`, never summed) and `index`, and the blend factor `alpha` for drawing. |
| `Clock::set_max_game_delta` | `core/clock.hpp` | The catch-up limit: a frame counts for at most four steps' worth of game time, so a stall slows the whole game rather than the simulation alone. |
| `FixedTickContext` | `core/frame_context.hpp` | What a subsystem sees in a step: the step's `dt`, `time`, `index`, how many of the frame's steps are left, and no `Clock`. |
| `Subsystem::fixed_tick` | `core/subsystem.hpp` | A subsystem's part of a step. The engine's `play_fixed_` group runs, per step: the environment's fixed half, scripts, physics. |
| `Engine::run_fixed_steps` | `core/engine.cpp` | The step loop: step input, `Scene::begin_fixed_step`, the group, collision dispatch, spawns and destroys settled. |
| `Scene::interpolate` | `scene/scene.cpp` | After the steps: what the last step moved is drawn blended between its start and end. |

### What the simulation reads

`Node3D::transform` is the simulation's state, exact, written only by
simulation code, physics read-back and `teleport()`. The simulation reads
placements through `Scene::current_world_matrix()`, built from those
transforms, and time from the step. It never reads `world_matrix_` or
`interpolated_transform_`, which are for drawing and lag the simulation by up
to a step, nor `game_elapsed`, which is summed from frames. Wind gusts are a
function of the step's time (`wind_gust_at`); the sea's lag behind the wind,
the derived wave field and the wakes advance per step in
`environment/environment_fixed.cpp`; buoyancy samples the sea at the step's
time.

### Targets set once a frame

Some things are decided once a frame and consumed by several steps:

- **Kinematic bodies and bone bodies** placed in `tick()` or by animation.
  `PhysicsWorld::step()` gets `frame_steps_left` and covers an even share of
  the way to the target on each step, so a frame of two steps moves the body
  on both. A kinematic node moved inside the step (its transform differs from
  `step_start_transform_`) reaches its target that step.
- **Character intent.** `move_intent` is cleared before input each frame;
  `PhysicsSubsystem::keep_frame_intents()` keeps what the frame set before its
  steps, and after each step physics puts it back, so the frame's intent steers
  every step and an intent a `fixed_tick()` set steers only its own.
- **Input edges.** `StepInputLatch` records the frame's presses and releases
  and hands each to exactly one step, the first to begin after it.

These stay frame-quantised on purpose — input arrives per frame — but nothing
the simulation does with them depends on how the steps are grouped.

### Determinism

Jolt is deterministic given the same calls in the same order and the same
binary. The engine keeps its side ordered:

- Bodies are created, and forces and targets applied, in scene order on the
  calling thread; buoyancy's parallel sampling writes its forces back in
  scene order.
- Contact callbacks arrive from Jolt's worker threads in whatever order they
  finish. Each step's events are sorted by body, other body, contact point and
  normal (`sort_events`) before scripts see them.
- One collision pass per sixtieth of a second (`collision_steps_for`), fixed
  for a session because the step length is.

### How it is held to that

- **Types.** `FixedTickContext` has no clock or frame delta; a
  `FixedStep`'s input is only reachable inside `fixed_tick()`.
- **`cmake/FixedTickCheck.cmake`** fails the build on a rendered matrix or a
  summed game time in any file in `cmake/fixed_tick_sources.txt`. A file that
  joins the simulation joins that list.
- **Write reports.** In the editor, a script whose `tick()` or `late_tick()`
  moves a dynamic body or writes a character's velocity outside its movement
  mode is reported once, and fails a functional test.
- **`tests/test_simulation_determinism.cpp`** plays one scene at several frame
  rates and uneven frames and requires every step to match to the bit, and
  compares contact order on one Jolt thread and on four.
- **`--run-tests --pacing`** does the same for a game's own functional tests.

## Planned sections
- Why Jolt — fast, deterministic, modern
- World setup — broad-phase layers, object layers, motion types
- Body shapes — box, sphere, capsule, mesh, convex hull
- `RigidbodyComponent` / `ColliderComponent`
- Queries — raycast, sphere cast, overlap
- Triggers / sensors
- Constraints (current support level)
- Debug visualization (debug draw integration)
- Limitations and roadmap
