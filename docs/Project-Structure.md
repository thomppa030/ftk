# Project Structure

## Repository Layout

```
Fjell/
  CMakeLists.txt                    Root build config
  cmake/
    Dependencies.cmake              FetchContent declarations
    CompileShaders.cmake            GLSL -> SPIR-V compilation
  shaders/
    mesh.vert                       PBR vertex shader
    mesh.frag                       PBR fragment shader (Cook-Torrance)
    debug.vert                      Collider wireframe vertex shader
    debug.frag                      Collider wireframe fragment shader
  assets/
    fonts/                          Inter, JetBrains Mono
    textures/                       Default textures
  src/
    main.cpp                        Entry point
    core/                           Engine core
    renderer/                       Vulkan rendering backend
    scene/                          Scene graph and transforms
    physics/                        Jolt Physics integration
    scripting/                      Component system and script loading
    ui/                             ImGui integration
    hub/                            Launcher application
```

## Source Directories

### `src/core/`

Engine lifecycle, window management, input, timing.

| File | Purpose |
|------|---------|
| `engine.hpp/cpp` | Main loop, editor UI, play mode |
| `window.hpp/cpp` | GLFW window wrapper |
| `input.hpp/cpp` | Keyboard/mouse state tracking |
| `clock.hpp/cpp` | Frame timing (delta_time, elapsed) |
| `project.hpp/cpp` | Project manifest and directory management |
| `play_session.hpp/cpp` | Play mode state (scene snapshot, physics, fly camera) |
| `layer.hpp` | Input layer interface |
| `layer_stack.hpp/cpp` | Priority stack for input event routing |
| `game_builder.hpp/cpp` | Package game for distribution |
| `log.hpp/cpp` | spdlog initialization, console sink |
| `base_path.hpp/cpp` | Resolve asset paths relative to executable |

### `src/renderer/`

Vulkan rendering backend.

| File | Purpose |
|------|---------|
| `vulkan_context.hpp/cpp` | Frame orchestration, command recording, debug pipeline |
| `device.hpp/cpp` | Instance, physical/logical device, queues |
| `swapchain.hpp/cpp` | Swapchain images, views, depth buffer |
| `offscreen_pass.hpp/cpp` | Offscreen render target for viewport |
| `pipeline.hpp/cpp` | PBR graphics pipeline |
| `model.hpp/cpp` | glTF/primitive mesh loading |
| `mesh.hpp/cpp` | GPU vertex/index buffers |
| `material.hpp/cpp` | PBR material with texture descriptor set |
| `texture.hpp/cpp` | Image loading and GPU upload |
| `image.hpp/cpp` | VkImage wrapper with layout transitions |
| `buffer.hpp/cpp` | VkBuffer wrapper (vertex, index, uniform, staging) |
| `descriptor.hpp/cpp` | Descriptor set layout and pool wrappers |
| `camera.hpp/cpp` | Orbit camera (editor) |
| `fly_camera.hpp/cpp` | First-person camera (play mode) |
| `primitives.hpp/cpp` | Cube, plane, sphere mesh generation |
| `resource_handle.hpp` | Generational handle template (Handle<Tag>) for type-safe resource refs |
| `resource_manager.hpp/cpp` | Handle-based resource pools (models, textures, materials) |
| `render_queue.hpp/cpp` | Sorted draw command list, built each frame by VulkanContext |
| `debug_draw.hpp/cpp` | Collider wireframe visualization |
| `render_stats.hpp` | Per-frame draw call and triangle tracking |
| `shader_watcher.hpp/cpp` | inotify-based GLSL hot-reload (debug only) |
| `uniform_types.hpp` | GlobalUbo, PbrPushConstants, DebugPushConstants, DirectionalLight |
| `vertex.hpp` | Vertex layout (position, color, texcoord, normal, tangent) |

### `src/scene/`

| File | Purpose |
|------|---------|
| `scene.hpp/cpp` | Scene graph: add/remove/find objects, hierarchy, save/load |
| `transform.hpp` | TRS struct with matrix computation and decomposition |

### `src/physics/`

| File | Purpose |
|------|---------|
| `physics_types.hpp` | BodyType, ColliderShape enums, RigidbodyDesc, ColliderDesc, RaycastHit |
| `physics_world.hpp/cpp` | Jolt wrapper: body creation, stepping, forces, raycast |

### `src/scripting/`

| File | Purpose |
|------|---------|
| `fjell_api.h` | Pure C header — the public scripting API |
| `script_manager.hpp/cpp` | .so loading, component lifecycle, auto-inspector |
| `json_bridge.hpp/cpp` | C-compatible JSON serialization bridge |

### `src/ui/`

| File | Purpose |
|------|---------|
| `imgui_layer.hpp/cpp` | ImGui Vulkan backend setup |
| `theme.hpp` | Shared amber theme (sRGB-to-linear conversion) |
| `console.hpp/cpp` | spdlog sink for in-editor log panel |
| `nvim_panel.hpp/cpp` | Embedded neovim via libvterm + PTY |
| `file_browser.hpp/cpp` | Filesystem browser widget for project open/create |

### `src/hub/`

| File | Purpose |
|------|---------|
| `main.cpp` | Hub launcher entry point |
| `hub_ui.hpp/cpp` | Project browser, splash screen |
| `recent_projects.hpp/cpp` | ~/.config/fjell/hub.json persistence |
