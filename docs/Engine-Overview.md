# Engine Overview

## Architecture

Fjell is structured as a set of cooperating systems, all owned by the `Engine` class:

```
Engine
  ├── Window              GLFW window
  ├── Input               Keyboard/mouse state
  ├── Clock               Frame timing
  ├── Camera              Orbit camera (editor)
  ├── VulkanContext        Rendering orchestrator
  │     ├── Device        Vulkan instance + device
  │     ├── Swapchain     Presentation images
  │     ├── OffscreenPass Viewport render target
  │     ├── Pipeline      PBR graphics pipeline
  │     └── DebugDraw     Collider wireframes
  ├── ImGuiLayer          Editor UI rendering
  ├── ResourceManager     Model/texture/material cache
  ├── Scene               Scene graph (objects, hierarchy)
  ├── ScriptManager       Component loading + update
  ├── NvimPanel           Embedded code editor
  ├── PlaySession (opt)   Play mode state
  │     ├── FlyCamera     First-person camera
  │     └── PhysicsWorld  Jolt simulation
  └── Project (opt)       Active project config
```

## Initialization Sequence

```
1. Log system init
2. Window creation (GLFW)
3. Vulkan device setup (instance, physical device, logical device)
4. Swapchain creation
5. VulkanContext (descriptors, pipeline, offscreen pass, command buffers, sync)
6. ImGui layer (Vulkan backend, fonts, theme)
7. Resource manager (default textures, primitive cache)
8. Scene load (from file or default)
9. Script loading (dlopen .so, register component types)
10. Enter main loop
```

## Shutdown

Reverse order. `vkDeviceWaitIdle()` before destroying Vulkan objects. Scene cleared before resource manager (which owns the GPU buffers). ImGui layer destroyed before Vulkan context releases the viewport texture.

## Two Executables

- **`fjell`** — The full editor + runtime. Can also run in game mode (standalone, no editor UI) when a `game.fjell` config is found next to the binary.
- **`fjell-hub`** — Lightweight project launcher. Minimal Vulkan setup (no scene, no physics, no scripting). Forks the editor process when a project is selected.

## Conventions

- Namespace: `fjell`
- `snake_case` for functions/variables, `PascalCase` for types, `UPPER_CASE` for constants
- Member variables suffixed with `_`
- `[[nodiscard]]` on pure getters
- RAII everywhere — no raw `new`/`delete`
- `#pragma once` for headers
- `-Werror` — warnings are errors
- VkResult must be checked (via `vk_check()` or manual check)
