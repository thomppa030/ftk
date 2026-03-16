<p align="center">
  <img src="assets/logo.svg" alt="Fjell" width="120"/>
</p>

<h1 align="center">Fjell</h1>

<p align="center">
  Vulkan game engine in C++23 for Linux.
</p>

---

## What is this

A game engine I'm building from scratch. Vulkan 1.3 renderer with PBR shading, scene graph, Jolt physics, C++ scripting with hot-reload, and an editor with embedded neovim. There's also a hub launcher for managing projects.

Still very much in progress.

## Getting it running

You'll need:
- Linux (I'm on Arch, other distros should work)
- A C++23 compiler (GCC 13+ or Clang 17+)
- CMake 3.25+
- Vulkan SDK installed system-wide
- libvterm (`pacman -S libvterm` on Arch)

Everything else gets pulled in automatically by CMake.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
```

Run the editor:
```bash
./build/src/fjell
```

Or the project launcher:
```bash
./build/src/fjell-hub
```

## What's in here

- **Renderer** — Vulkan 1.3 dynamic rendering, Cook-Torrance PBR, offscreen viewport, shader hot-reload
- **Scene** — Parent-child hierarchy, transform gizmos (ImGuizmo), JSON save/load
- **Physics** — Jolt Physics. Rigidbodies, box/sphere/capsule/mesh colliders, raycasting, wireframe debug view
- **Scripting** — Write components in C++, compile to `.so`, engine loads them at runtime. Fields are reflected for auto-generated inspector UI and serialization. Hot-reload in debug builds
- **Editor** — ImGui with docking, property inspector, asset browser, console, stats
- **Code editor** — Embedded neovim via libvterm
- **Hub** — Project launcher with recent projects list

## Dependencies

All fetched via CMake FetchContent except Vulkan SDK and libvterm:

| | |
|---|---|
| GLFW 3.4 | Windowing |
| GLM 1.0.1 | Math |
| spdlog 1.15.0 | Logging |
| Dear ImGui (docking) | UI |
| ImGuizmo | Gizmos |
| nlohmann/json 3.11.3 | JSON |
| fastgltf 0.8.0 | glTF loading |
| stb_image | Textures |
| Jolt Physics 5.2.0 | Physics |

## Docs

Architecture, scripting guides, and API reference are in [docs/](docs/Home.md).
