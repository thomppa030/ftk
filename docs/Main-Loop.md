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
    ├─ 3. Game Logic (play mode only)
    │   play_session_->update()           FlyCamera movement
    │   script_manager_.update()          Component tick() callbacks
    │   play_session_->physics_step()     Jolt step + transform sync
    │   check wants_stop() → stop_play()
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
    │   if (is_playing())
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

Entering play mode (`start_play()`):
1. Clear undo history
2. Serialize all component data to JSON (for restore)
3. Create `PlaySession` — snapshots the scene, creates `PhysicsWorld`, bodies
4. Prepare scripts (begin_play callbacks)
5. Capture cursor for FPS camera
6. Deselect any object

Exiting play mode (`stop_play()`):
1. Call end_play on all scripts
2. Restore scene from snapshot (transforms, objects, hierarchy)
3. Deserialize component data back from JSON
4. Release cursor
5. Destroy `PlaySession` (and with it, `PhysicsWorld`)

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
