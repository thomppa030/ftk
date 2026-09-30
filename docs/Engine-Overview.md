# Engine Overview

Fjell is a Vulkan game engine and editor built in C++23 for Linux and Windows. It ships two executables:

- **fjell** — the editor and runtime. You build scenes, attach scripts, hit play, and iterate.
- **fjell-hub** — a launcher for managing projects.

## Architecture

```
Engine
  ├── Window                 SDL window + surface
  ├── Input                  Keyboard and mouse state
  ├── Clock                  Frame delta time and elapsed time
  ├── ThreadPool             Worker threads for parallel work
  ├── VulkanContext           Rendering orchestrator (all passes, particles, debug drawing)
  ├── ImGuiLayer             Editor UI rendering
  ├── ResourceManager        Handle-based pools for models, textures, materials (bindless)
  ├── Scene                  Object list, hierarchy, lights
  ├── ScriptManager          Loads and runs script components
  ├── CommandHistory          Undo/redo stack
  │
  ├── UI Panels
  │     ├── ViewportManager   Multiple viewports, each showing a different visualization
  │     ├── HierarchyPanel    Scene tree with add, delete, drag-to-reparent
  │     ├── PropertiesPanel   Inspector for transforms, physics, lights, components
  │     ├── StatsPanel        FPS, draw calls, triangle counts, GPU info
  │     ├── HistoryPanel      Visual undo/redo history
  │     ├── ContentBrowser    File browser with PNG icon thumbnails
  │     └── IconCache         Loads and caches icon textures for the content browser
  │
  ├── PlaySession (optional)
  │     ├── FlyCamera         First-person camera for play mode
  │     └── PhysicsWorld      Jolt Physics simulation
  ├── Project (optional)     Active project configuration
  └── ShaderWatcher (debug)  Watches GLSL files via inotify and triggers hot-reload
```

## Initialization Sequence

```
1. Log system init
2. Window creation (SDL)
3. Vulkan device setup (instance, physical device, logical device)
4. Swapchain + VulkanContext (all render passes, descriptors, sync)
5. ImGui layer (Vulkan backend, fonts, theme)
6. Resource manager (default textures, bindless setup)
7. Icon cache (PNG icons for content browser)
8. Content browser, hierarchy, properties, stats, history panels
9. Scene load (from file or default)
10. Script loading (dlopen .so, register component types)
11. Viewport manager (first viewport)
12. Enter main loop
```

## Shutdown

Reverse order. `vkDeviceWaitIdle()` before destroying Vulkan objects. Icon cache destroyed before ImGui layer (since icons hold ImGui descriptors). Scene cleared before resource manager.

## Conventions

- Namespace: `fjell`
- `snake_case` for functions/variables, `PascalCase` for types, `UPPER_CASE` for constants
- Member variables suffixed with `_`
- `[[nodiscard]]` on pure getters
- RAII everywhere — no raw `new`/`delete`
- `#pragma once` for headers
- `-Werror` — warnings are errors
- Nothing outside the GPU backend names Vulkan ([GPU interface](Architecture/GPU-Interface.md)); inside it, every `VkResult` is checked with `vk_check()`
