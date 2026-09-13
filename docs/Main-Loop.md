# Main Loop

The main loop lives in `Engine::run()`. It runs until the window is closed.

## Frame Sequence

```
while (!window.should_close()) {
    ┌─ 1. Input
    │   input_.update()              Reset per-frame deltas
    │   window_.poll_events()        GLFW event dispatch
    │   clock_.tick()                Compute delta_time
    │
    ├─ 2. Shader Hot-Reload (debug only)
    │   shader_watcher_.poll()       inotify check on shaders/
    │   → reload_pipeline()          Recompile SPIR-V, recreate pipelines
    │
    ├─ 3. Game Logic (play mode only, gated on play_controller_.playing())
    │   animation_subsystem_->tick()      Pose before scripts and physics read it
    │   play_phase_0_                     Net receive, scripts, camera
    │   play_phase_1_                     Physics + collision dispatch
    │   play_phase_2_                     VFX
    │
    ├─ 4. Script Hot-Reload (debug only)
    │   script_manager_.poll_reload()  inotify check on game.so
    │
    ├─ 5. Scene Graph
    │   scene_.compute_world_matrices()    Propagate parent→child transforms
    │
    ├─ 6. Viewport Resize
    │   viewport_manager_.apply_resize()   Recreate offscreen passes if needed
    │
    ├─ 7. UI
    │   imgui_layer_->begin_frame()
    │   handle_play_shortcuts()            Ctrl+P toggles play, Shift+Esc stops
    │   if (play_controller_.playing())
    │     draw_play_overlay()              Full-window viewport + stop bar
    │   else
    │     draw_ui()                        Menu bar, dockspace, all panels
    │   imgui_layer_->end_frame()
    │
    └─ 8. Render
        Build viewport render requests (each viewport has its own camera + viz source)
        vulkan_context_.draw_frame(requests, light, &scene_, dt, selected_id, thread_pool)
}
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

## Physics Step Order

The ordering within a play-mode frame matters:

```
scripts run       →  scripts can apply forces, set velocities
physics step      →  Jolt integrates forces, resolves collisions
                     dynamic body transforms sync back to scene
world matrices    →  recomputed with updated positions
render            →  objects appear at their post-physics positions
```

Kinematic bodies are synced *into* Jolt before the step (engine→Jolt), so scripts can move kinematic objects and physics responds in the same frame.
