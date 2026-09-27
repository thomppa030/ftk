<p align="center">
  <img src="assets/logo.svg" alt="Fjell" width="120"/>
</p>

<h1 align="center">Fjell</h1>

<p align="center">
  Vulkan game engine in C++23 for Linux and Windows.
</p>

---

## What is this

A game engine I'm building from scratch. Vulkan 1.3 renderer with PBR shading and
global illumination, scene graph, Jolt physics, skeletal animation, a particle/VFX
system, networking, and C++ scripting with hot-reload. The editor runs on ImGui, and
a hub launcher manages projects.

Still very much in progress.

## Getting it running

The **Vulkan SDK** is the only thing you need to install yourself — it provides the
loader, the validation layers, and `glslc`, which the engine calls both at build time
and at runtime to compile materials. Get it from [LunarG](https://vulkan.lunarg.com/)
or your distro (`vulkan-devel` and `shaderc` on Arch).

Beyond that:
- A C++23 compiler — GCC 13+, Clang 17+, or MSVC 2022
- CMake 4.3+ and Ninja (both ship with Visual Studio's C++ workload on Windows)
- On Linux, X11 development headers (`xorg-dev` on Debian/Ubuntu, `libx11` on Arch)

Everything else — 22 libraries, including physics, animation, and font rendering — is
fetched and built by CMake. That makes the first build a long one.

```bash
cmake --preset debug
cmake --build --preset debug
```

`release`, `asan` (Linux only), and `tracy` presets are also available; run
`cmake --list-presets` to see them.

Run the editor:
```bash
./build/src/fjell                 # empty
./build/src/fjell /path/to/project
```

Or the project launcher, which is the easier way in:
```bash
./build/src/fjell-hub
```

## What's in here

- **Renderer** — Vulkan 1.3 dynamic rendering, Cook-Torrance PBR, a data-driven frame
  graph, mesh shaders with GPU culling, cascaded shadows, and DDGI/screen-space global
  illumination
- **Materials** — `.fjsl` spatial shaders in a Godot-like dialect, compiled at runtime,
  with hot-reload
- **Scene** — Parent-child hierarchy, transform gizmos, JSON save/load
- **Physics** — Jolt. Rigidbodies, box/sphere/capsule/mesh colliders, collision channels,
  raycasting, wireframe debug view
- **Animation** — Skeletal animation on ozz, state machines, blend spaces, notifies
- **VFX** — Node-graph particle editor, GPU simulation, mesh and lit particles
- **Game UI** — `.fjui`/`.fjss` markup and stylesheets with flexbox layout and MSDF text
- **Scripting** — Write components in C++, compile to a shared library, engine loads it
  at runtime. Fields are reflected for inspector UI and serialization. Hot-reload in
  debug builds
- **Networking** — Client/server over ENet with replication and prediction
- **Editor** — ImGui with docking, property inspector, asset browser, console, profiler
- **Hub** — Project launcher with a recent projects list

## Dependencies

The Vulkan SDK is the only system dependency. Everything below is fetched and built
by CMake:

| | |
|---|---|
| SDL 3.4 | Window, keyboard, mouse and gamepads |
| GLM 1.0.1 | Math |
| spdlog 1.15.0 | Logging |
| Dear ImGui (docking) | Editor UI |
| ImGuizmo | Gizmos |
| nlohmann/json 3.11.3 | Serialization |
| fastgltf 0.8.0 | glTF loading |
| stb_image | Textures |
| Jolt Physics 5.2.0 | Physics |
| ozz-animation 0.15.0 | Skeletal animation |
| meshoptimizer 0.22 | Meshlets and LOD generation |
| Yoga 3.2.1 | Flexbox layout for game UI |
| FreeType + msdfgen 1.12 | Font atlases |
| miniaudio 0.11.21 | Audio |
| ENet 1.3.18 | Networking |
| Assimp 5.4.3 | Mesh import (`fjimport` tool only) |
| Tracy 0.13.1 | Profiling (opt-in) |
| VMA 3.2.1 | Vulkan memory allocation |
| Catch2 3.8.0 | Tests |

## Docs

Architecture, scripting guides, and API reference are in [docs/](docs/Home.md).
