# ftk

ftk is a toolkit for desktop programs with a GPU and an editor-style UI,
written in C++23: an SDL window and a GPU interface with a Vulkan backend, a
frame graph, Dear ImGui drawn through it with a theme and a kit of widgets,
undo, a console, and the plumbing such programs share. It began as the
foundation of the Fjell engine and is meant for programs like it: editors,
tools and creative apps. It builds on Linux and Windows.

## Libraries

Each library is a CMake target, `ftk::<name>`, and includes nothing of a
library it does not link; the build checks that.

| Library | What it holds |
|---|---|
| `ftk::base` | The log, results, delegates, handles, the thread pool, the profiler, string and UTF-8 helpers |
| `ftk::math` | Colour space conversions and keyed curves |
| `ftk::platform` | Paths, processes, file watching and shared libraries, one backend per OS |
| `ftk::gpu` | The GPU interface and the window; its Vulkan backend, `src/ftk/gpu/vulkan/`, is the only code that names Vulkan |
| `ftk::gpu-imgui` | Dear ImGui drawn through the GPU interface |
| `ftk::framegraph` | Pass ordering, barriers, transient images and the import catalogue |
| `ftk::shader` | GLSL to SPIR-V through `glslc`, kept by content in a cache, with glslc's errors read back |
| `ftk::image` | Image loading, saving and resizing (stb) |
| `ftk::app-ui` | The editor theme, its icons and the kit of widgets |
| `ftk::app` | The ImGui layer, standalone windows, undo and its history panel, the console, the file browser, the icon cache and editor contexts |
| `ftk::imgui-harness` | Dear ImGui with no window, for a program's tests to drive its UI |

## Building

You need CMake 4.3 or newer, Ninja, a C++23 compiler (CI builds with GCC 13
and MSVC 2022) and the Vulkan SDK, whose `glslc` compiles the shaders. On
Linux, SDL builds its video and input drivers from the X11, Wayland and udev
headers listed in `.github/actions/linux-toolchain/action.yml`. Everything
else is fetched at configure.

    cmake --preset debug
    cmake --build --preset debug
    ctest --test-dir build

Built on its own, ftk builds its tests too: `ftk-tests`, a link check per
library and a benchmark of the command list. `-DFTK_ENABLE_TRACY=ON` adds
Tracy profiler zones.

## Using it in a program

Fetch it at a tag:

```cmake
include(FetchContent)
FetchContent_Declare(ftk
    GIT_REPOSITORY https://github.com/thomppa030/ftk.git
    GIT_TAG        v0.1.0)
FetchContent_MakeAvailable(ftk)

target_link_libraries(my_program PRIVATE ftk::app)
ftk_stage_editor_data(my_program)
```

`ftk_stage_editor_data()` puts the theme's fonts and the ImGui layer's
fragment stage in `ftk/` beside the program's binary, and the program hands
those paths to its ImGui layer. When working on ftk and a program at once,
point the fetch at a checkout with `-DFETCHCONTENT_SOURCE_DIR_FTK=<path>`.

A program can hold its own code to the rules ftk keeps. Each check runs at
configure and again before the target builds, and fails with the file and
the line:

- `ftk_check_editor_ui(<target> ROOTS <dirs>... [ALLOWLIST <file>] [ICONS <files>...])`:
  editor UI takes its colours from the theme and its widgets from the kit,
  never styled by hand
- `ftk_check_gpu_backend(<target> ROOTS <dirs>...)`: nothing outside the
  backend names Vulkan
- `ftk_check_portability(<target> ROOTS <dirs>...)`: nothing outside the
  platform library uses POSIX-only headers or calls

## Fonts

`fonts/` holds the theme's fonts, each with its licence beside it: Geist and
Geist Mono, Inter and JetBrains Mono under the SIL Open Font License, and the
Lucide icons under ISC.
