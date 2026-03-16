# Main Loop

The main loop lives in `Engine::run()` (`src/core/engine.cpp`). It runs until the window is closed.

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
    │   → reload_pipeline()          Recompile SPIR-V, recreate pipeline
    │
    ├─ 3. Game Logic
    │   if (is_playing())
    │     play_session_->update()           FlyCamera movement
    │     script_manager_.update()          Component Update callbacks
    │     play_session_->physics_step()     Jolt step + transform sync
    │     check wants_stop() → stop_play()
    │   else
    │     camera_.update()                  Orbit camera (editor)
    │
    ├─ 4. Script Hot-Reload (debug only)
    │   script_manager_.poll_reload()  inotify check on game.so
    │
    ├─ 5. Scene Graph
    │   scene_.compute_world_matrices()    Propagate parent→child transforms
    │
    ├─ 6. UI
    │   imgui_layer_->begin_frame()
    │   if (is_playing())
    │     draw_play_overlay()              Full-window viewport + stop bar
    │   else
    │     draw_ui()                        Dockspace, panels, menus
    │   imgui_layer_->end_frame()
    │
    └─ 7. Render
        Compute view/projection from active camera
        vulkan_context_.draw_frame(view, proj, cam_pos, light, &scene_)
}
```

## Play Mode

Entering play mode (`start_play()`):
1. Serialize all component data to JSON (for restore)
2. Create `PlaySession` — snapshots the scene, creates `PhysicsWorld`, bodies
3. Capture cursor for FPS camera
4. Deselect any object

Exiting play mode (`stop_play()`):
1. Restore scene from snapshot (transforms, objects, hierarchy)
2. Deserialize component data back from JSON
3. Release cursor
4. Destroy `PlaySession` (and with it, `PhysicsWorld`)

In game mode (`game.fjell` present), play starts immediately and ESC quits the application instead of returning to the editor.

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
