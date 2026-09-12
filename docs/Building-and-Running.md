# Building & Running

## Prerequisites

The **Vulkan SDK** is the only dependency you install yourself. It provides the loader,
the validation layers, and `glslc` — which the engine needs twice over: once to compile
its own shaders at build time, and again at runtime to compile `.fjsl` materials. A
machine running a prebuilt engine still needs it.

| | |
|---|---|
| Vulkan SDK | [LunarG](https://vulkan.lunarg.com/), or `vulkan-devel` + `shaderc` on Arch |
| C++23 compiler | GCC 13+, Clang 17+, or MSVC 2022 |
| CMake 4.3+ | Bundled with Visual Studio's C++ workload |
| Ninja | Also bundled with that workload |
| X11 headers (Linux) | `xorg-dev` on Debian/Ubuntu, `libx11` on Arch |

Everything else is fetched by CMake. That is 22 libraries, several of which — Assimp,
Jolt, ozz, msdfgen, FreeType — build from source, so expect the first configure and
build to take a while. Later builds reuse `build/_deps`.

On Windows the SDK installer does not put `%VULKAN_SDK%\Bin` on `PATH`. That is fine:
the build locates `glslc` through the SDK itself.

## Build

Use the presets — they pin Ninja, so Linux and Windows build the same way and
`CMAKE_BUILD_TYPE` behaves consistently.

```bash
cmake --preset debug
cmake --build --preset debug
```

| Preset | What it is |
|---|---|
| `debug` | Validation layers, shader and script hot-reload. `build/` |
| `release` | Optimized. `build-release/` |
| `asan` | Debug plus AddressSanitizer and UBSan. Linux only. `build-asan/` |
| `tracy` | RelWithDebInfo with the Tracy profiler client. `build-tracy/` |

`cmake --list-presets` shows the ones available on your platform. The `asan` preset
reuses the `debug` tree's imgui checkout, so configure `debug` first.

## Run

```bash
./build/src/fjell                      # editor, no project
./build/src/fjell /path/to/project     # editor with a project
./build/src/fjell-hub                  # project launcher
```

The hub is the easier entry point: it lists recent projects and can create new ones.

## Tests

```bash
ctest --test-dir build --output-on-failure
```

## Installing

```bash
cmake --install build --prefix /somewhere
```

The install tree is self-contained — editor, hub, tools, shaders, assets, and the
scripting SDK headers — and projects built against it resolve the SDK from
`<prefix>/sdk`. `cpack` produces a `.zip` on Windows and a `.tar.gz` elsewhere.
