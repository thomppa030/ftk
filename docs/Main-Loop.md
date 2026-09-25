# Main Loop

The main loop lives in `Engine::run()`. It runs until the window is closed.

## Frame Sequence

```
while (!window.should_close()) {
    ┌─ 1. Input and time
    │   input_.update(), window_.poll_events()
    │   clear every character's move_intent        (play only)
    │   input_action_map_.update()                 Action handlers fire here
    │   clock_.tick()                              Frame and game delta
    │   play_controller_.begin_frame()             Pause/step bookkeeping, step input
    │
    ├─ 2. Hot reload (debug only): shaders, frame graphs, LUTs
    │
    ├─ 3. Play (only while play_controller_.playing())
    │   animation                                  Pose before scripts and physics read it
    │   play_phase_0_                              Net receive, scripts' tick(), camera, UI
    │   play_phase_1_                              Environment, the frame half
    │   run_fixed_steps()                          0, 1 or several fixed steps (below)
    │   scene_.interpolate(alpha)                  Blend what the steps moved
    │   renderer time = render_time()
    │   scripts' late_tick()                       Cameras and followers
    │   play_phase_2_                              VFX
    │
    ├─ 4. Test runner, script build and hot reload
    │
    ├─ 5. Always-on subsystems, then compute_world_matrices()
    │
    ├─ 6. UI: editor panels, or the play overlay
    │
    └─ 7. Render
        One request per viewport, each with its own camera and view
}
```

Each fixed step inside `run_fixed_steps()`:

```
step input latched for this step
scene_.begin_fixed_step()                          Keep every node's transform
a waiting functional test starts
play_fixed_: environment's fixed half              Wind, sea wind, water, wakes
             scripts' fixed_tick() and timers
             physics                               Forces, targets, intent, Jolt, read back
collision and trigger callbacks                    Sorted, dispatched
spawned and destroyed nodes settled
```

## Multi-Viewport Rendering

Each viewport panel produces a `ViewportRenderRequest` with its own camera, projection, and visualization source (final color, depth, wireframe, normals, etc.). The renderer processes all requests in a single `draw_frame` call. Viewports that aren't visible skip rendering.

## Play Mode

`PlayController` (`core/play_controller.hpp`) owns the play lifecycle. Engine
asks it whether the game is running; the scene editor draws the toolbar and
overlay and reacts to `on_started` / `on_stopping` / `on_stopped`. Nothing else
starts or stops play.

`start()`:
1. Capture a `PlaySnapshot`: the scene through `Scene::to_json` (the same
   serializer that writes `.fjell` files, so every registered component and
   every script field comes back), plus the input context stack and the set of
   script-created scene viewports
2. Start physics and VFX with no physics world in their context
3. Start camera, audio, sprite animation; load input contexts; start scripts
   (`begin_play`) and UI widgets; load input contexts again for anything
   `begin_play` spawned
4. Create the `PlaySession`, tell the renderer, fire `on_started` (the editor
   clears undo history and the selection)

`stop()`:
1. Fire `on_stopping`, reset the clock, clear script input bindings
2. Stop UI, camera, audio, sprite animation, scripts (`end_play`), VFX
3. `PlaySnapshot::restore`: rebuild the scene from JSON, re-create the
   editor-side script instances, pop input contexts pushed during play,
   destroy viewports created during play, clear screen debug text
4. Stop physics, destroy the session, release the cursor, fire `on_stopped`

Shortcuts: Ctrl+P toggles play, Shift+Esc stops. ESC is never intercepted, so
a game can bind it. In game mode (`game.fjell` present) there is no snapshot;
play starts immediately and `stop()` closes the window.

### Pause, step, time scale

Backed by `Clock`, not separate controller state — a script calling
`fjell_time_set_paused`/`fjell_time_set_scale` shows up in the toolbar and
status bar the same as the editor's own controls, since both read the same
clock. `pause()`/`resume()` also tell `AudioSubsystem` to freeze and thaw
every tracked node source in place (one-shot sounds are deliberately left
running — they have no node to resume against).

`step()` marks the session as stepping and calls `resume()`; the next call to
`PlayController::begin_frame()` (from `Engine::run`, right after `clock_.tick()`
and before the play phases tick) asks for exactly one fixed step and re-pauses
at the start of the frame after, so one step of the simulation advances,
whatever that frame's time was.

While paused, frames go on: `tick()` and `late_tick()` still run, with a delta
of zero, and no fixed step does.

Shortcuts: Ctrl+Shift+P toggles pause, Ctrl+Alt+P steps once. The
viewport toolbar shows pause and step buttons only while playing, plus a
time-scale drag field (0.1–4×).

## The fixed step

The simulation — physics, what scripts do in `fixed_tick()`, timers, the
environment's simulated half — advances in steps of one length, 1/60 s unless
the project sets another rate (Project Settings > Simulation, 10 to 240 Hz,
read when play starts). Frames take however long they take. `FixedTimestep`
(`core/fixed_timestep.hpp`) adds up each frame's game time and runs as many
whole steps as it covers, carrying the rest to the next frame: at 144 fps most
frames run none, at 60 one, at 30 two.

Because every step is the same length, the simulation comes out the same at
any frame rate, to the bit, and can be replayed from the same inputs.
`tests/test_simulation_determinism.cpp` holds the engine to that, and
`--run-tests --pacing` holds a game to it (see
[Functional Tests](Scripting/Functional-Tests.md)).

**The catch-up limit.** A frame may count for at most four steps' worth of game
time (`FixedTimestep::MAX_STEPS_PER_FRAME`). A longer frame — a hitch, a load, a
debugger stop — is cut to that by the `Clock` before any step runs, so the
whole game slows down through it together instead of trying to catch the
simulation up in one frame and making the next one slower still. At 60 Hz that
means below 15 fps the game runs slow. The log says so once when it starts,
and Stats > Simulation counts the time it cost.

**What is drawn.** A node the last step moved is drawn blended between where
that step started and ended it, by how far the frame sits past the step
(`alpha`), so motion is smooth at any frame rate. The blend goes in
`Node3D::interpolated_transform_`; `transform` stays the exact simulation
state and nothing in the simulation reads the blend. Position, rotation and
scale blend separately, and each only while it is still where the last step
left it: a script turning a character in `tick()` doesn't stop its walk from
blending. `teleport()` moves a node without a blend. The renderer's time, and
so `TIME` in shaders, is `render_time()`, the same point in the step, so waves
are drawn at the moment the hulls floating on them are. Drawing therefore
trails the simulation by up to one step.

**Three script hooks.** `tick()` runs once a frame before the steps, so what
input and gameplay decide this frame the steps see; `fixed_tick()` runs in
each step; `late_tick()` runs after the blend, for cameras and anything that
follows what is on screen. [Scripting Overview](Scripting/Overview.md#the-three-ticks)
says what goes where.

**Subsystems.** A subsystem that is part of the simulation overrides
`Subsystem::fixed_tick(const FixedTickContext&)` and joins `play_fixed_`.
`FixedTickContext` carries the step's `dt`, `time` and `index` and has no
`Clock` and no frame delta, so the simulation cannot read frame time by
mistake. `cmake/FixedTickCheck.cmake` refuses a rendered matrix or a summed
game time in any file listed in `cmake/fixed_tick_sources.txt`.
