# Fjell Engine

Vulkan game engine written in C++23, targeting Linux.

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
```

Release build:
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## Run

```bash
./build/src/fjell
```

## Project structure

- `src/` — Engine source code
  - `core/` — Engine core (window, main loop, input, clock)
  - `renderer/` — Vulkan rendering backend
    - `device` — Instance, debug messenger, surface, physical/logical device, queues
    - `swapchain` — Swapchain, image views, depth buffer, render pass, framebuffers
    - `pipeline` — Pipeline layout + graphics pipeline (dynamic viewport/scissor)
    - `vulkan_context` — Composes Device/Swapchain/Pipeline; owns descriptors, command buffers, sync, frame orchestration
    - `shader_watcher` — inotify-based GLSL hot-reload (debug builds only)
    - `render_stats` — Per-frame draw call and triangle count tracking
  - `scene/` — Scene graph and transforms
  - `ui/` — ImGui integration
- `shaders/` — GLSL shaders (compiled to SPIR-V at build time into `build/shaders/`)
- `cmake/` — CMake modules (dependency fetching, shader compilation)

## Conventions

- Namespace: `fjell`
- Naming: `snake_case` for functions/variables, `PascalCase` for types, `UPPER_CASE` for constants
- Member variables suffixed with `_`
- Use `[[nodiscard]]` on pure getters and functions where ignoring the return value is a bug
- Prefer RAII — no raw `new`/`delete`
- Headers use `#pragma once`
- Compiler warnings treated as errors (`-Werror`)
- Vulkan API calls that return `VkResult` must be checked — use `vk_check()` from `renderer/vk_check.hpp`

## Dependencies

All fetched via CMake FetchContent:
- GLFW 3.4 — windowing
- GLM 1.0.1 — math
- spdlog 1.15.0 — logging
- Dear ImGui (docking branch) — debug UI
- stb_image — texture loading
- fastgltf 0.8.0 — glTF model loading
- Vulkan SDK — system install required
