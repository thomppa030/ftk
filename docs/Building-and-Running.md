# Building & Running

## Prerequisites

- Linux (tested on Arch)
- C++23 compiler (GCC 13+ or Clang 17+)
- CMake 3.25+
- Vulkan SDK (system install)
- libvterm (for embedded neovim panel)

All other dependencies are fetched automatically via CMake FetchContent.

## Build

```bash
# Debug (validation layers, shader hot-reload, script hot-reload)
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)

# Release
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## Run

```bash
# Editor (no project)
./build/src/fjell

# Editor with project
./build/src/fjell /path/to/project

# Hub launcher
./build/src/fjell-hub
```

## Dependencies

Fetched automatically:

| Library | Version | Purpose |
|---------|---------|---------|
| GLFW | 3.4 | Windowing |
| GLM | 1.0.1 | Math |
| spdlog | 1.15.0 | Logging |
| Dear ImGui | docking | Editor UI |
| ImGuizmo | latest | Transform gizmos |
| nlohmann/json | 3.11.3 | Serialization |
| fastgltf | 0.8.0 | glTF model loading |
| stb_image | latest | Texture loading |
| Jolt Physics | 5.2.0 | Physics simulation |

System dependencies:

| Library | Purpose |
|---------|---------|
| Vulkan SDK | Rendering API + glslc shader compiler |
| libvterm | Terminal emulation for neovim panel |
