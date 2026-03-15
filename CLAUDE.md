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
  - `core/` — Engine core (window, main loop)
  - `renderer/` — Vulkan rendering backend
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

## Dependencies

All fetched via CMake FetchContent:
- GLFW 3.4 — windowing
- GLM 1.0.1 — math
- Vulkan SDK — system install required
