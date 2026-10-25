# ftk

ftk is a toolkit for desktop programs with a GPU and an editor-style UI,
written in C++23: an SDL window and a GPU interface with a Vulkan backend, a
frame graph, an immediate-mode UI of its own drawn through it with a theme
and a kit of widgets, undo, a console, and the plumbing such programs share. It began as the
foundation of the Fjell engine and is meant for programs like it: editors,
tools and creative apps. It builds on Linux and Windows.

## Libraries

Each library is a CMake target, `ftk::<name>`, and includes nothing of a
library it does not link; the build checks that.

| Library | What it holds |
|---|---|
| `ftk::base` | The log, results, delegates, handles, the thread pool, the profiler, string and UTF-8 helpers, compression (zstd) |
| `ftk::math` | Colour space conversions and keyed curves |
| `ftk::platform` | Paths, processes and their pipes, the program's own standard streams, file watching and shared libraries, one backend per OS |
| `ftk::jsonrpc` | JSON-RPC as the language server protocol carries it: messages framed by length, a peer talking over two byte streams, file URIs and positions along a line |
| `ftk::gpu` | The GPU interface and the window; its Vulkan backend, `src/ftk/gpu/vulkan/`, is the only code that names Vulkan |
| `ftk::framegraph` | Pass ordering, barriers, transient images and the import catalogue |
| `ftk::shader` | GLSL to SPIR-V through `glslc`, kept by content in a cache, with glslc's errors read back |
| `ftk::image` | Image loading, saving and resizing (stb), encoding to BC4, BC5, BC6H and BC7 (CMP_Core), and drawing SVG files (lunasvg) |
| `ftk::text` | Font faces, fallback chains, shaping and glyph rasterising (FreeType, HarfBuzz) |
| `ftk::ui-core` | The immediate-mode UI: ids and kept state, layout, input, OS windows and popups, draw lists; no GPU code |
| `ftk::ui-kit` | The kit's headers, the editor theme and its icons |
| `ftk::app-ui` | The kit of widgets drawn by the UI core, and its gallery |
| `ftk::gpu-ui` | The UI core's frames drawn through the GPU interface |
| `ftk::app` | The main window, or a layer for a program running its own loop, its input and undo; pictures made textures, and SVG files drawn at the pixels they cover |
| `ftk::app-editor` | Standalone windows, undo's history panel, the console, the file browser, the icon cache and editor contexts |
| `ftk::ui-harness` | The UI core with no window, to step frames and feed it the mouse and keys |
| `ftk::kit-harness` | That harness with the kit and its fonts, for a program's tests to drive its UI |

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

`ftk_stage_editor_data()` puts the theme's fonts in `ftk/fonts/` beside the
program's binary, and the program hands that path to `ui::use_kit_fonts()`.
When working on ftk and a program at once,
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

`fonts/` holds the theme's fonts, each with its licence beside it: Geist, Inter
and JetBrains Mono under the SIL Open Font License, and the Lucide icons under
ISC.
