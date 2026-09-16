# Next Engine — Architecture Guidelines

A focused brief for starting over. Distilled from what worked in Fjell, what cost real performance, and what cost months of retrofitting. Each section ends with sources for further reading — papers, talks, and reference codebases. Read these *before* committing to a design, not after.

---

## 0. Non-negotiables on day one

These five decisions reshape every line of code after them. Pick them up front. Adding any of them later means rewriting the renderer.

1. **GPU-driven rendering.** The CPU never builds a draw list. Scene change → patch GPU buffer. Frame → indirect dispatch.
2. **DAG framegraph with per-dispatch nodes.** Not per-pass. Aliasing and barrier batching depend on this.
3. **Multiple queue islands modeled as data.** Graphics, async compute, transfer, RT — pass declares queue; graph emits CB segments. No three-CB execute signature.
4. **Bindless everything.** One descriptor set for textures + materials + buffers. No per-material sets, ever.
5. **Resource ownership lives in the graph.** Passes never own VkImage/VkBuffer. Persistent = a flag on `create()`, not a separately-managed pool.

Skip any one of these and you'll be retrofitting for a year.

**Sources:**
- Wihlidal, *Optimizing the Graphics Pipeline with Compute* (GDC 2016) — the seed talk for GPU-driven.
- O'Donnell, *FrameGraph: Extensible Rendering Architecture in Frostbite* (GDC 2017) — the canonical framegraph paper, still the best one.
- Hodes, *Halcyon + Vulkan* (GDC 2018) — practical bindless and modern Vulkan in production.
- Sascha Willems Vulkan samples ([github.com/SaschaWillems/Vulkan](https://github.com/SaschaWillems/Vulkan)) — reference for every modern Vulkan feature.

---

## 1. The core loop

```
begin_frame()    → wait for slot N's fence, acquire image
build_frame()    → graph compile (cached), import live persistents
execute()        → record CB segments per queue
submit()         → submit each segment with timeline syncs, present
```

That's the entire frame surface. If you have a 1700-line `VulkanContext` you went off the path. Target: ~200 lines for the frame driver, everything else lives in the graph, the passes, or the GPU device.

**One frame slot owns:** command buffers (one per queue type), fences, semaphores, per-frame descriptor pool, uniform buffer slice. Nothing else.

**No central "renderer" object** that holds 40 pass pointers. Passes live in a registry, you look them up by name only at config time (loading the graph, editor UI). Hot path never touches strings.

**Sources:**
- Vulkan-Samples ([github.com/KhronosGroup/Vulkan-Samples](https://github.com/KhronosGroup/Vulkan-Samples)) — read `framework/core/` for a clean device/swapchain split.
- *Writing an Efficient Vulkan Renderer* — Zeux Kapoulkine ([zeux.io/2020/02/27/writing-an-efficient-vulkan-renderer](https://zeux.io/2020/02/27/writing-an-efficient-vulkan-renderer/)). Probably the single best practical Vulkan article.

---

## 2. Framegraph design

### 2.1 Resources

- One resource ID type per kind: `FgTexture { uint32_t }`, `FgBuffer { uint32_t }`.
- Resource name is for *authoring* (`.fjgraph` files, editor display). At runtime everything is an index into a flat array. **No string lookups in `record()`.**
- A resource has lifecycle states: `Transient` (graph owns, aliased), `Persistent` (graph owns, survives `begin_frame`), `Imported` (external, graph tracks layout only). `Imported` should be rare — swapchain images and that's about it.

### 2.2 Per-dispatch nodes

The DAG node is a single barrier-boundary unit of work, not "the bloom pass." A pass that does 6 dispatches contributes 6 nodes. This is the difference between 9% aliasing and 50% aliasing.

Implementation: `record()` calls `frame.dispatch(...)` / `frame.draw(...)` which the graph treats as a barrier point. Passes declare *what they touch in each phase*, not "what the pass touches overall."

### 2.3 Caching

Compile once, execute many. The compiled DAG (sort order, barrier list, alias group assignments, CB segment topology) is a function of: pass set, enable flags, viewport extent, sample count, format set. Cache it. Invalidate on change. Most frames are cache hits.

### 2.4 Barriers

- Use `VK_KHR_synchronization2` exclusively. No legacy barriers.
- Batch: walk consecutive same-queue nodes, emit one `vkCmdPipelineBarrier2` per barrier-boundary.
- Cross-queue: timeline semaphore. Do not overpromote stages to `ALL_COMMANDS` — track queue capability per stage flag and split barriers if needed.

### 2.5 Queue model

```cpp
enum class Queue : uint8_t { Graphics, AsyncCompute, Transfer };
```

Pass declares its queue in `setup()`. Graph splits the linearized DAG into runs of same-queue passes (*segments*). Each segment gets a CB. Adjacent segments on different queues sync via a timeline semaphore. There is no "before compute / after compute" — there are N segments.

**Sources:**
- O'Donnell, *FrameGraph* (Frostbite, GDC 2017).
- Hans-Kristian Arntzen, *Render graphs and Vulkan — a deep dive* ([themaister.net/blog/2017/08/15](https://themaister.net/blog/2017/08/15/)). Specifically the resource-aliasing and barrier sections.
- *granite* engine, Arntzen's own ([github.com/Themaister/Granite](https://github.com/Themaister/Granite)) — read `vulkan/render_graph.cpp`. The reference implementation.
- *Modern Rendering Architecture* — Riccio (GDC 2019) on per-dispatch granularity.
- `VK_KHR_synchronization2` spec section in the Vulkan spec. The examples appendix is gold.

---

## 3. GPU-driven scene

### 3.1 The draw stream is GPU-owned

A persistent `GpuDrawData[]` buffer indexed by stable draw ID. CPU touches only dirty entries (transform changed, material changed, mesh swapped). The frame's visible set is built by a compute pass that reads scene state and writes indirect commands. **There is no `IndirectBatch::build` walking a CPU vector every frame.**

### 3.2 Culling

- Frustum + Hi-Z occlusion in compute. Two-pass: cull → render → re-cull rejects against fresh Hi-Z.
- LOD selection in the same shader using projected bounding sphere area.
- Per-meshlet cone backface culling.

### 3.3 Mesh shaders only — but with a fallback path you can drop later

Vulkan 1.3+ + `VK_EXT_mesh_shader`. The vertex-shader fallback exists for two months while you bring up tooling, then it dies. Don't ship both indefinitely.

### 3.4 Skinning on the GPU

Bone matrices in an SSBO, the mesh/vertex shader applies them. **Skinned meshes go into the BLAS** — write the deformed positions to a scratch buffer once per frame and rebuild/refit the BLAS from that. If you skip this, characters don't appear in RT reflections, which is what happened in Fjell.

**Sources:**
- Wihlidal, *Optimizing the Graphics Pipeline with Compute* (GDC 2016).
- Haar & Aaltonen, *GPU-Driven Rendering Pipelines* (Siggraph 2015) — the AC: Unity talk that made GPU-driven mainstream.
- meshoptimizer docs ([github.com/zeux/meshoptimizer](https://github.com/zeux/meshoptimizer)) — read the meshlet build and cone culling sections.
- Schied, *Mesh Shader Possibilities* (NVIDIA, 2020).
- *niagara* — Zeux's mesh-shader renderer ([github.com/zeux/niagara](https://github.com/zeux/niagara)). Small, readable, ships a complete GPU-driven mesh-shader pipeline.

---

## 4. Materials and shaders

### 4.1 One material model

A material = shader handle + parameter block + texture references. All materials use the same descriptor set (bindless). Per-shader pipeline variant cached on first use. **Do not** create descriptor sets per-material. Ever.

### 4.2 Authored shaders, not hand-written

Godot-style spatial shaders worked well in Fjell. Keep that idea:
- A small DSL extends a built-in pipeline (writes to `ALBEDO`, `ROUGHNESS`, etc).
- Compile to GLSL at load, then to SPIR-V via `glslc`/`shaderc` library.
- Uniform hints (`hint_color`, `hint_range`) drive editor UI for free.
- Vertex and fragment stages from day one. Fjell's omission of vertex hooks blocked water/Gerstner waves.

### 4.3 Hot reload as a first-class system

One asset hot-reload bus. Inotify on Linux. Every asset type subscribes. **Don't write per-asset-type watchers** (Fjell has 5 separate watcher classes). One watcher, dispatch by extension.

### 4.4 SPIR-V management

- Pipeline cache to disk, keyed by GPU + driver version.
- Async pipeline compile. Never block a frame on pipeline creation. Use a fallback (black, magenta) for in-flight compiles.
- `VK_EXT_graphics_pipeline_library` if you go heavy on shader variants.

**Sources:**
- Godot's `shader_language.cpp` for inspiration on the DSL ([github.com/godotengine/godot](https://github.com/godotengine/godot)).
- *Slang* shading language ([shader-slang.com](https://shader-slang.com/)) — consider it as the authored layer. Has SPIR-V and reflection built-in.
- Khronos *Shader Tutorial* and `glslang`/`shaderc` for online compile.
- Karis, *Real Shading in Unreal Engine 4* (Siggraph 2013) — still the reference for PBR parameterization.

---

## 5. Bindless

### 5.1 What "bindless" actually means

One descriptor set. Layout:
- Binding 0: large `SAMPLED_IMAGE` array (`MAX_TEXTURES` ~16k+).
- Binding 1: `materials[]` SSBO indexed by material ID.
- Binding 2: `draw_data[]` SSBO indexed by `gl_DrawID`.
- Binding 3: `lights[]` SSBO.
- (Optionally) Binding 4: `samplers[]` array if you need varied samplers.

Update with `VK_EXT_descriptor_indexing` (core in 1.2): `PARTIALLY_BOUND`, `UPDATE_AFTER_BIND`, `nonuniformEXT`.

### 5.2 What it isn't

Bindless is *not* "no descriptor sets." You still have one global set bound at frame start. Don't write per-pass descriptor pools, frame caches, or "descriptor set acquire" APIs. Fjell has a `FrameDescriptorCache` because passes still allocate sets — that's a smell.

### 5.3 Slot management

- Texture handle = bindless slot index. Generational, like asset handles.
- Slot 0 reserved for the "missing" texture. Magenta or checker.
- Free slots reused immediately; generations bump on reuse.

**Sources:**
- Hodes, *Halcyon + Vulkan* (GDC 2018).
- Sascha Willems, *Vulkan descriptor indexing example* in his sample repo.
- `VK_EXT_descriptor_indexing` and `VK_EXT_descriptor_buffer` specs.
- Wicked Engine notes on bindless ([wickedengine.net/2021/04/06/bindless-descriptors](https://wickedengine.net/2021/04/06/bindless-descriptors/)).

---

## 6. Scene representation

### 6.1 Don't write a hierarchical scene graph for rendering

A scene *graph* is a tree of nodes with parent transforms. That's an editor concept. The *renderer* wants flat arrays: `transforms[]`, `aabbs[]`, `mesh_refs[]`, `material_refs[]`, `flags[]`, all indexed by a stable entity ID.

Transform resolution (parent → world) happens once per frame on dirty entities, output into the flat array. The renderer then sees: AoS hierarchy is gone, SoA flat arrays remain.

### 6.2 ECS-shaped storage even if you don't ship an ECS

You don't need EnTT/flecs. You need:
- Stable entity IDs (generational).
- Component arrays indexed by ID.
- Dirty bits per component for incremental update.

This is also what makes the GPU-driven draw stream cheap: a moved entity flips one dirty bit, you patch one buffer offset, you submit.

### 6.3 Animation

Skeletal animation runtime: ozz-animation is the reference. Don't write your own. Authored as `.fjanim`/`.fjskel` (engine-native binary). Importer from `.glb`/`.fbx` lives in a separate tool.

**Sources:**
- Acton, *Data-Oriented Design* (CppCon 2014). Watch it.
- Fabian Giesen, *Texture tiling and swizzling* and his other posts on memory layout ([fgiesen.wordpress.com](https://fgiesen.wordpress.com/)).
- ozz-animation ([guillaumeblanc.github.io/ozz-animation](https://guillaumeblanc.github.io/ozz-animation/)).
- *The Game Object Model* — Stefan Reinalter's blog ([blog.molecular-matters.com](https://blog.molecular-matters.com/)).

---

## 7. Asset pipeline

### 7.1 Native binary formats only

`.fjmesh`, `.fjskel`, `.fjanim`, `.fjtex`, etc. — your formats, your version field, no runtime dependency on Assimp/fastgltf/FreeType in the engine binary.

Tools (separate executables) convert third-party formats. Engine loads only native.

### 7.2 Text in development, binary in shipping

`.fjmat`, `.fjgraph`, `.fjui` stay JSON/text for diff-ability and editor authoring. Materials and graphs are sparse and human-edited. Meshes/textures/animations are dense and binary.

### 7.3 Async loading by default

Background CPU work (parse, decompress, build meshlets) → main-thread GPU upload (because Vulkan resource creation is single-threaded for memory bind). Returns a handle in `Loading` state; renderer skips that draw until `Ready`.

### 7.4 Texture streaming

If you want it, plan for it on day one. Sparse images (`VK_KHR_sparse_*`) or virtual texturing. Adding streaming later means revisiting every texture API.

**Sources:**
- *Granite* texture compression pipeline ([github.com/google/granite](https://github.com/google/granite) — note: different project from Arntzen's renderer).
- KTX2/Basis Universal ([github.com/KhronosGroup/KTX-Software](https://github.com/KhronosGroup/KTX-Software)). Use this format.
- meshoptimizer's vertex/index codec — read the docs even if you don't use the codec.
- Aaltonen, *Optimizing the Graphics Pipeline with Compute* — covers virtual texturing decisions.

---

## 8. Editor as a separate concern

### 8.1 The renderer doesn't know about the editor

No `OverlayCallback`, `GameUiCallback`, `WorldUiCallback`, `UiPreviewCallback` (Fjell has all four). The renderer exposes:
- "Here are the views being rendered this frame."
- "Here's the swapchain image after compositing."

The editor sits *on top*. It owns ImGui, owns its draw, calls into the renderer to record an editor pass at the end. The renderer has no notion of "selected entity" or "viewport offset in window pixels."

### 8.2 One "view" type

A view = camera + render target + graph filename. The main window is a view that happens to present. Editor previews, security cameras, mirrors, minimaps — all the same type, no special-case `SceneViewport` next to a main-viewport code path.

### 8.3 Render to texture is the only path

If the swapchain image *is* a render target, then "render-to-texture for a material" and "render to the window" use the same path. Fjell has two paths; that's why camera previews skip RT reflections.

**Sources:**
- *Dear ImGui* docking branch ([github.com/ocornut/imgui](https://github.com/ocornut/imgui)) — read the multi-viewport example.
- Unreal's `FViewport` and `FSceneRenderer` split (read the source if you have access).

---

## 9. Reflection and tooling

### 9.1 Codegen, not macros

Pick a path on day one: clang AST → JSON → templated headers, or a custom parser, or libclang. Don't write per-component `_meta.cpp` files by hand. By the time you have 30 components, the cost of writing them is dwarfed by the cost of *forgetting* to update one.

### 9.2 What reflection is for

- Serialization (scene save/load).
- Editor property panels.
- Script bindings.
- Diff/merge for scene files.

It is *not* for runtime polymorphism. Reflection in a game engine is a compile-time artifact.

### 9.3 Scripting

Pick one language. C# (with .NET hosting), Lua (sol2), or your own. C++ as a scripting language is a trap — hot-reload of native code on Linux is a minefield.

**Sources:**
- *Magnum Engine* `Corrade::Utility` reflection approach.
- *EnTT* meta system ([github.com/skypjack/entt](https://github.com/skypjack/entt)) — runtime reflection done right if you go that way.
- *.NET 8 hosting APIs* — if you want C# without Mono.
- *sol2* ([github.com/ThePhD/sol2](https://github.com/ThePhD/sol2)) for Lua.
- Jonathan Blow's *Jai* talks on hot-reload — even if you don't use Jai, the constraints are educational.

---

## 10. Performance discipline

### 10.1 Profile from day one

- Tracy ([github.com/wolfpld/tracy](https://github.com/wolfpld/tracy)) for CPU + GPU timeline. Wire it in week one.
- Per-pass GPU timing via `VK_KHR_calibrated_timestamps`.
- Per-frame allocation counter (every `new`/`malloc` counted in debug).

### 10.2 Per-frame allocation budget: zero

The hot path allocates *nothing* per frame. All per-frame buffers reuse capacity. Use arenas/bump allocators for one-frame scratch. Containers in hot code use stack storage (`std::array`, `inplace_vector` if available, or a small-buffer-optimized vector).

### 10.3 No `std::function` in the per-frame path

`std::function` allocates above SBO and defeats inlining. Use function pointers, `inplace_function`, or direct virtual calls. The framegraph's `add_pass(name, std::function<...>)` API is the wrong shape — passes implement `record(Frame&)` directly.

### 10.4 No `std::unordered_map` keyed on strings in hot code

Pre-hash at register time, key on `uint64_t`. Or better: indices into flat arrays. `std::string` as a hot-path key is a perf bug.

### 10.5 Synchronization

- Timeline semaphores for queue-to-queue.
- Binary semaphores only for swapchain acquire/present.
- One fence per frame slot, nothing else.
- Never `vkDeviceWaitIdle` in the hot path. Even on resize, prefer per-frame fence waits.

**Sources:**
- Zeux, *Writing an Efficient Vulkan Renderer*.
- *RAD Game Tools* blog posts on profiling discipline (Casey Muratori, Sean Barrett).
- Wicked Engine's perf notes ([wickedengine.net](https://wickedengine.net/)).
- Adam Sawicki's VMA blog ([asawicki.info](https://asawicki.info/)) — author of VMA, his Vulkan posts are excellent.

---

## 11. What to skip in v1

Cut ruthlessly. The features that nearly broke Fjell were not features — they were features added *before the core was stable*.

**Skip:**
- A custom UI framework. Use Dear ImGui for editor and game until v2. Authored UI (HTML-like, CSS-like) is a year of work that competes with the renderer.
- VFX node editor.
- Animation state-machine editor.
- Multiple scripting languages.
- A custom IDE / code editor.
- Physics until you have a game that needs it.
- Audio until you have a game that needs it.
- More than one rendering backend. Vulkan-only. Don't pretend you'll add D3D12 someday.

**Keep:**
- Renderer.
- Scene + ECS-shaped storage.
- Asset pipeline (mesh + texture + material).
- One scripting language with hot reload.
- Editor chrome via ImGui.
- A two-test scene: hero PBR mesh + lit environment, animated character with skinned mesh.

When the two-test scene runs at 240 FPS with all the post-process you care about, *then* think about the next system.

---

## 12. Tech stack recommendation

Two viable stacks. Pick by language — both ship.

### 12.1 C++23 stack

| Layer | Pick | Why |
|---|---|---|
| Build | CMake + FetchContent or Meson | What everyone else uses. Don't fight the ecosystem. |
| Vulkan loader | volk | Avoids the loader trampoline, faster function calls. |
| Memory | VMA | Don't write an allocator. |
| Math | GLM or your own SIMD lib | GLM is fine for v1. |
| Logging | spdlog | Or roll a 200-line version. |
| ImGui | docking branch | Editor only. |
| Mesh import (tool) | Assimp or fastgltf | Tool, not engine. |
| Mesh optimization | meshoptimizer | Industry standard. |
| Skeletal anim | ozz-animation | Industry standard. |
| Font atlas | msdfgen + FreeType | If you ship text. |
| Profiler | Tracy | Day one. |
| Hot reload | inotify (Linux) / ReadDirectoryChangesW (Win) | Roll your own bus. |
| Reflection | clang-AST codegen or libclang | Don't write per-component meta files by hand. |

### 12.2 Rust stack

The Rust ecosystem is genuinely production-ready for a Vulkan engine in 2026. Single-developer ergonomics are arguably better than C++23: no header/cpp split, Cargo replaces CMake-and-praying, type system catches a class of GPU-resource-lifetime bugs that previously needed runtime validation layers. The price you pay is `winit 0.30`'s `ApplicationHandler` shape, occasional FFI for tools, and slower iteration if you don't set up code hot-reload.

| Layer | Pick | Why / notes |
|---|---|---|
| Vulkan bindings | **`ash`** | Raw, 1:1 with C API. What Embark, kajiya, rend3 use. Avoid `vulkano` (wrong abstraction level for you), `wgpu` (not Vulkan), `erupt` (dead). |
| Memory | **`gpu-allocator`** (Traverse Research) | Pure-Rust, no C++ build dep, well-maintained. VMA-class strategies. Skip `vk-mem` bindings — fragmented forks. |
| Window | **`winit 0.30+`** | The default. Plan for `ApplicationHandler` trait + `run_app` — `EventLoop::run` is gone. Your `Engine` struct *is* the handler. |
| Math | **`glam`** | SIMD f32, fastest in mathbench, what every other game crate expects. `nalgebra` is for scientific code. |
| ECS | **`bevy_ecs`** (standalone, no rest of Bevy) | Most active, archetypal, parallel scheduler, change detection. Pairs with `bevy_reflect`. `hecs` for minimalist; `legion` is dead. |
| Reflection | **`bevy_reflect`** | Replaces hand-written `_meta.cpp` files entirely. `#[derive(Reflect)]` gives runtime introspection for editor inspectors + serialization. This single crate is one of the strongest reasons to consider Rust. |
| Serialization | **`serde`** | Pair with `Reflect`. JSON for authored assets, bincode/postcard for native binary. |
| Threading | **`rayon`** + **`crossbeam`** | rayon for data-parallel (cull, anim, particles); crossbeam for channels. No async runtime unless netcode. |
| Logging | **`tracing`** + **`tracing-tracy`** + **`tracing-subscriber`** | Spans become Tracy zones automatically. Bridge old `log` users via `tracing-log`. |
| Profiler | Tracy via `tracing-tracy` | Same Tracy server you'd use from C++. |
| ImGui | **`imgui-rs`** (docking branch) | If you want C++ ImGui mental model. **`egui`** if you want pure-Rust and accept rebuilding some dense editor widgets — docking is weaker, tables thinner. For an editor-heavy engine, imgui-rs still wins. |
| Shader compile (build) | shell out to **`glslc`** from `build.rs` | Same as C++. Emit `cargo:rerun-if-changed=` for incremental. |
| Shader compile (runtime) | **`shaderc`** crate | In-process libshaderc for hot-reload. Skip `naga` — its GLSL frontend is incomplete and Vulkan-flavor GLSL extensions are spotty. |
| Mesh import | **`gltf`** + **`meshopt`** | glTF crate is canonical; meshopt has zeux's bindings with the same API as the C++ lib. |
| FBX | **`russimp`** in importer tool | No native Rust FBX exists at production quality. Bind Assimp in the importer; keep FBX out of the engine. |
| Skeletal anim | **`ozz-animation-rs`** | From-scratch Rust port (not a binding) of ozz runtime. Deterministic, cross-platform — good for rollback netcode. Use ozz's C++ offline tools to bake. |
| File watch | **`notify`** v8+ | Used by rust-analyzer. Use the debounced API — raw events fire too eagerly. |
| Code hot-reload | **`hot-lib-reloader`** (defer) | Split gameplay into a dylib, reload on rebuild. **`subsecond`** (from Dioxus) is the new hotness but rougher. Defer code hot-reload until asset hot-reload via `notify` stops being enough. |
| Gamepad | **`gilrs`** | What everyone uses. |

### 12.3 Cargo workspace shape

```
my_engine/
├── crates/
│   ├── engine/          # the engine (lib + bin)
│   ├── render/          # framegraph, passes, gpu-driven mesh
│   ├── scene/           # ECS components, scene format
│   ├── assets/          # loaders, hot-reload bus
│   ├── editor/          # ImGui editor (separate from engine!)
│   └── fjimport/        # mesh importer tool
├── shaders/             # .glsl, .fjsl
└── Cargo.toml           # workspace
```

`engine` depends on `render`, `scene`, `assets`. `editor` depends on `engine`. **`render` and `scene` never depend on `editor`.** Enforce with `cargo deny` and a workspace doc.

### 12.4 Stack comparison — when to pick which

| Concern | C++23 | Rust |
|---|---|---|
| Vulkan API surface | Bare metal, you write everything | Bare metal via `ash`, identical |
| Build setup pain | CMake hell | Cargo + `build.rs`, trivially better |
| Reflection / editor inspector | Write codegen or libclang | `bevy_reflect` for free |
| Hot-reload of game code | Painful (Build & Reload class of bugs) | `hot-lib-reloader` is purpose-built |
| Iteration speed (compile times) | Faster incremental | Slower; mitigate with `cargo-nextest`, `mold`, workspace splits |
| Hiring / community | Larger pool | Smaller but enthusiastic; game-dev pool is growing fast |
| Lifetime / borrow checker | None | Real cost for renderer code — Vulkan handles fight the borrow checker until you commit to `Arc<Device>` or unsafe boundaries |
| GPU resource bug class | Caught by validation layers at runtime | Some caught at compile time, but Vulkan's lifetime model is *frame-scoped* which doesn't map cleanly to Rust lifetimes |
| Editor UI | ImGui docking just works | imgui-rs works; egui forces rewrites |
| FBX / niche format support | Mature C++ libs | Bind via `russimp` in tools only |

**Honest recommendation:** if you have deep C++ Vulkan reflexes from Fjell, **stay in C++23** — your second engine will ship faster. If you've wanted to make the jump for a while and accept ~3 months of "fighting the borrow checker around `VkDevice`," **Rust gives you a cleaner long-term codebase**, free reflection, and a build system that won't make you cry. There is no third option that's better than both.

**Avoid in both:** anything that wraps Vulkan in a portable abstraction layer (bgfx, sokol, wgpu, vulkano). You want direct Vulkan to learn it and to use mesh shaders / RT / extensions on your terms.

---

## 13. Order of work (first 90 days)

This sequence keeps the engine *running and visible* at every step. No "now we rewrite the renderer to support meshlets" stall.

**Days 1–14 — Bring-up.**
Window, instance, device, swapchain, one triangle. Tracy wired. Sync2. VMA. Bindless layout declared (even if empty). Frame loop with timeline semaphores.

**Days 15–30 — Framegraph.**
Per-dispatch nodes, compiled cache, barrier batching. One persistent transient pool. Three test passes: clear, blit, present. Aliasing analysis runs and logs lifetimes.

**Days 31–50 — Scene + GPU-driven mesh.**
Flat scene storage. Mesh format. Meshlet build. Compute cull. Mesh shader pipeline. Render a stress scene of 10k unique meshes at >120 FPS.

**Days 51–70 — Material + lighting.**
Authored shader DSL. PBR pipeline. Directional light + cascaded shadows. One IBL probe. Validate on a Sponza-class scene.

**Days 71–90 — Editor + asset pipeline.**
ImGui docking editor. Inspector via reflection codegen. Hot reload bus. Mesh importer tool. Save/load a scene.

After day 90 you have a baseline real renderer. Everything after — GI, RT reflections, volumetric fog, post-process — slots into the existing graph. None of it should require structural changes.

---

## 14. Reading list, in priority order

If you read these five things before writing any code, you'll save months:

1. **Zeux Kapoulkine — *Writing an Efficient Vulkan Renderer*** ([zeux.io/2020/02/27/writing-an-efficient-vulkan-renderer](https://zeux.io/2020/02/27/writing-an-efficient-vulkan-renderer/)). Read it twice.
2. **Yuriy O'Donnell — *FrameGraph: Extensible Rendering Architecture in Frostbite*** (GDC 2017). The framegraph paper. Slides on GDC Vault.
3. **Hans-Kristian Arntzen — *Render graphs and Vulkan***, blog series at themaister.net. Then read *Granite*'s source.
4. **Mike Acton — *Data-Oriented Design*** (CppCon 2014). For how to lay out scene data.
5. **Aaltonen / Haar — *GPU-Driven Rendering Pipelines*** (Siggraph 2015). For why every modern engine looks the way it does.

After those: Sascha Willems' samples, Wihlidal's GDC talks, the Vulkan spec's synchronization examples appendix.

---

## 15. What "done" looks like for v1

- Sponza + an animated character at 4K, 240 FPS on a mid-range GPU, with: cascaded shadows, GTAO, bloom, tonemap, TAA.
- Scene save → quit → load → identical visual result, sub-second load.
- Edit a shader, save, see the change in <500 ms with no engine restart.
- Add a new render pass by writing one file and registering it in a `.fjgraph`. No core changes.
- Total engine LOC under 30k. (Fjell is north of 100k.)

If those five hold, you have a foundation. If any of them required workarounds, fix it before adding the next feature.
