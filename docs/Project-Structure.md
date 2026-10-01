# Project Structure

Quick map of the source tree. If you're looking for something specific, this should tell you where to find it.

## Source Code — `src/`

### `src/core/` — Engine core

| File | What it does |
|---|---|
| `engine` | Main loop, owns all subsystems, ticks everything |
| `window` | SDL window creation, events and the Vulkan surface |
| `input` | Keyboard and mouse state polling |
| `clock` | Frame delta time and elapsed time |
| `project` | Project configuration (paths, settings) |
| `play_controller` | Play mode lifecycle: state machine, subsystem start/stop order, transition delegates |
| `play_session` / `play_snapshot` | State alive only while playing, and the editor scene captured for restore |
| `game_builder` | Builds standalone game packages |
| `base_path` | Resolves paths relative to the executable |
| `commands` | Concrete command implementations (transform, rename, add/remove, reparent, lights) |

### `src/ftk/` — ftk, the toolkit

What the engine is built on and other GUI programs can use: libraries declared
in `src/libraries.cmake`, each linked as `ftk::<name>`, that know nothing of
the engine.

| Directory | Library | What it holds |
|---|---|---|
| `ftk/base/` | `ftk::base` | The log, results, delegates, handles, the thread pool, the profiler, string and UTF-8 helpers |
| `ftk/math/` | `ftk::math` | Colour space conversions and keyed curves |
| `ftk/platform/` | `ftk::platform` | Paths, processes, file watching and shared libraries, one backend per OS |
| `ftk/gpu/` | `ftk::gpu`, `ftk::gpu-imgui` | The GPU interface, the SDL window and Dear ImGui drawn through it; its Vulkan backend is `ftk/gpu/vulkan/`, and nothing else names Vulkan. See [GPU interface](Architecture/GPU-Interface.md) |
| `ftk/framegraph/` | `ftk::framegraph` | Pass ordering, barriers, transient images and the import catalogue |
| `ftk/shader/` | `ftk::shader` | GLSL to SPIR-V through `glslc`, kept by content in a cache, and glslc's errors read back |
| `ftk/image/` | `ftk::image` | Image loading, saving and resizing (stb) |
| `ftk/ui/` | `ftk::app-ui` | The editor theme, the icons and the kit of widgets (`ftk/ui/kit/`) |
| `ftk/app/` | `ftk::app` | The ImGui layer, standalone windows, undo and its history panel, the console, the file browser, the icon cache and the `EditorContext` base |

### `src/renderer/` — Renderer

**Core rendering:**

| File | What it does |
|---|---|
| `vulkan_context` | Top-level rendering orchestrator — owns all passes, runs the frame |
| `pipeline_cache` | Per-shader pipeline variants — compiles `.fjsl` at runtime |
| `offscreen_pass` | Off-screen render target for viewports |

**Resources:**

| File | What it does |
|---|---|
| `resource_manager` | Handle-based pools for models, textures, materials; `.fjmat` loading |
| `resource_handle` | Generational `Handle<Tag>` template |
| `model` | glTF model loading via fastgltf |
| `mesh` | Vertex/index buffer data |
| `meshlet` / `meshlet_buffer` / `meshlet_descriptor` | Meshlet generation, GPU buffers, and descriptor setup |
| `material` | PBR material with texture references, pipeline ID, custom params |
| `material_asset` | `.fjmat` JSON load/save |
| `texture` | Texture loading and Vulkan image creation |
| `image` | VkImage wrapper |
| `buffer` | VkBuffer wrapper |
| `mega_buffer` | Large shared buffer for bindless vertex/index data |
| `bindless` | Bindless descriptor set — texture array + material SSBO + custom params SSBO |
| `descriptor` | Descriptor set/pool helpers |
| `draw_data_descriptor` | Per-draw data descriptor layout |
| `fjsl_parser` | Parse `.fjsl` shaders, extract metadata, generate GLSL from spatial template |
| `fjsl_loader` | Read a `.fjsl` through the project and the engine, compose the functions it uses, generate its fragment |
| `pipeline_cache` | Per-shader VkPipeline cache (opaque + transparent + mesh shader per `.fjsl`) |

**Rendering pipeline:**

| File | What it does |
|---|---|
| `render_queue` | Sorted draw command list, rebuilt each frame |
| `indirect_batch` | Multi-draw indirect batch building |
| `camera` | Camera matrices (view, projection) |
| `fly_camera` | First-person fly camera for play mode |
| `particle_system` | GPU-driven particle emission, simulation, and rendering |
| `debug_draw` | Immediate-mode debug line/shape drawing |
| `viz_image` | Shared image management for visualization passes |
| `primitives` | Built-in mesh generation (cube, sphere, plane) |

**Utilities:**

| File | What it does |
|---|---|
| `render_stats` | Per-frame draw call and triangle count tracking |
| `shader_watcher` | inotify-based GLSL/FJSL hot-reload (debug builds only) |
| `uniform_types` | Shared uniform buffer struct definitions |
| `vertex` | Vertex format definition |
| `viewport_source` | Enum for which visualization a viewport displays |
| `renderer_constants` | Named constants (max frames in flight, etc.) |

### `src/renderer/pass/` — Render passes

Each pass is its own class. They're instantiated and orchestrated by `VulkanContext`.

| Pass | What it does |
|---|---|
| `shadow_pass` | Cascaded shadow map rendering (3 cascades) |
| `depth_prepass` | Early Z pass (traditional vertex) |
| `mesh_shader_pass` | Main geometry via mesh shaders |
| `mesh_shader_depth_pass` | Depth-only mesh shader path |
| `transparent_pass` | Alpha-blended objects (back-to-front) |
| `sky_pass` | Procedural sky rendering |
| `bloom_pass` | Multi-stage bloom (downsample + upsample + composite) |
| `post_pass` | The post pass: the scene colour to what the viewport shows, through a `shader_type post` shader (`shaders/post.fjsl` by default) |
| `light_cull_pass` | Forward+ tiled light culling (compute) |
| `debug_pass` | Debug line rendering |
| `depth_viz_pass` | Depth buffer visualization |
| `normal_pass` | World-space normal visualization |
| `uv_viz_pass` | UV coordinate visualization |
| `wireframe_pass` | Wireframe overlay |
| `overdraw_pass` | Overdraw heatmap |
| `meshlet_viz_pass` | Meshlet boundary visualization |
| `light_viz_pass` | Light tile heatmap visualization |

### `src/scene/`

| File | What it does |
|---|---|
| `scene` | Object list, hierarchy, lights, save/load |
| `transform` | Position + quaternion rotation + scale, with matrix computation |

### `src/physics/`

| File | What it does |
|---|---|
| `physics_types` | Rigidbody and collider description structs |
| `physics_world` | Jolt Physics world wrapper |

### `src/scripting/`

| File | What it does |
|---|---|
| `fjell_api.h` | C header for field reflection types |
| `fjell_component` | Component base class — begin_play, tick, end_play |
| `fjell_world` | World interface exposed to scripts (input, physics, logging) |
| `fjell_registry` | Type registration macros and auto-registration |
| `script_manager` | Loads script libraries, manages component lifecycle |

### `src/ui/` — Editor panels

All panels use Dear ImGui. Each is a self-contained class.

| File | What it does |
|---|---|
| `viewport_manager` | Multiple viewports, each with its own viz source |
| `hierarchy_panel` | Scene tree — add, delete, drag-to-reparent |
| `properties_panel` | Object inspector — transform, physics, lights, material params, components |
| `stats_panel` | FPS, draw calls, GPU info |
| `content_browser` | File browser with icon thumbnails (overlay panel) |

## Shaders — `shaders/`

GLSL shaders compiled to SPIR-V at build time into `build/shaders/`. `.fjsl` shaders and include files are copied to `build/shaders/` for runtime compilation.

**Material shaders (`.fjsl`):**
- `standard.fjsl` — default PBR (empty fragment = standard PBR, pipeline ID 0)
- `tint.fjsl` — example custom shader with tint color and strength
- `include/fjell_spatial_template.glsl` — template that wraps user `.fjsl` code in the full PBR pipeline
- `include/fjell_pbr.glsl`, `fjell_shadow.glsl`, `fjell_lighting.glsl`, `fjell_common.glsl` — shared includes

**Main rendering:**
- `mesh.vert`, `mesh.frag` — traditional vertex pipeline
- `mesh.task`, `mesh.mesh` — mesh shader geometry path
- `depth.vert`, `depth_push.vert` — depth prepass variants
- `depth_mesh.task`, `depth_mesh.mesh` — mesh shader depth
- `shadow.vert` — shadow map rendering
- `sky.vert`, `sky.frag` — procedural sky

**Post-processing:**
- `bloom_downsample.frag`, `bloom_upsample.frag`, `bloom_composite.frag` — bloom chain
- `post.fjsl` — the engine's look: bloom and a tone curve, drawn by the post pass through `include/fjell_post_template.glsl`; the curves are in `include/tonemap/`

**Particles:**
- `particle_emit.comp`, `particle_sim.comp`, `particle_indirect.comp` — GPU particle compute
- `particle.vert`, `particle.frag` — particle rendering

**Debug and visualization:**
- `debug.vert`, `debug.frag` — debug line drawing
- `depth_viz.vert`, `depth_viz.frag` — depth visualization
- `normal_viz.vert`, `normal_viz.frag` — normal visualization
- `uv_viz.vert`, `uv_viz.frag` — UV visualization
- `wireframe.frag` — wireframe overlay
- `overdraw.frag` — overdraw heatmap
- `meshlet_viz.mesh`, `meshlet_viz.frag` — meshlet visualization
- `light_cull.comp` — tiled light culling
- `light_heatmap.vert`, `light_heatmap.frag` — light tile heatmap
- `outline.vert`, `outline.frag` — selection outline

## Assets — `assets/`

- `fonts/` — UI fonts (Inter, JetBrains Mono)
- `textures/` — Engine default textures
- `models/` — Built-in models
- `materials/` — `.fjmat` material asset files
- `icons/` — PNG icons for the content browser
