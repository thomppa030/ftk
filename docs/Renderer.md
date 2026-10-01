# Renderer

## Overview

Fjell uses Vulkan 1.3 with dynamic rendering — no `VkRenderPass` or `VkFramebuffer` objects. All rendering is done through `vkCmdBeginRendering`/`vkCmdEndRendering`, and image layout transitions use `VkImageMemoryBarrier2` (synchronization2).

MSAA is configurable (2x, 4x, or 8x). Multisampled color and depth are resolved before post-processing.

## Bindless Textures + Material System

All loaded textures live in a single descriptor array (set 1, binding 0). Materials are stored in an SSBO (set 1, binding 1) and reference textures by their array index. A second SSBO (set 1, binding 2) holds custom shader parameters as a flat `float[]` buffer. Shaders sample via `textures[nonuniformEXT(index)]` and read material properties via `materials[material_id]`.

There are no per-material descriptor sets. Every draw just pushes a `material_id`.

### GpuMaterial (96 bytes)

```
texture_indices[0-3]  — PBR textures (base_color, normal, metallic_roughness, emissive)
texture_indices[4-7]  — custom shader textures (up to 4 per material)
base_params[0-11]     — PBR params (base_color_factor, metallic, roughness, has_normal_map)
param_offset          — offset into custom params SSBO (in floats)
param_count           — number of custom floats
```

### Custom Shaders (.fjsl)

Godot-style spatial shaders. A `.fjsl` file declares uniforms and a `fragment()` body that writes to built-in outputs (`ALBEDO`, `ROUGHNESS`, `METALLIC`, `NORMAL`, `EMISSION`, `AO`, `ALPHA`). The FJSL parser generates a complete fragment shader from a template, the `ShaderCompiler` compiles it to SPIR-V via `glslc` (kept in `shaders/generated/.fjcache` by content, so an unchanged shader is not compiled twice), and the `PipelineCache` creates opaque + transparent + mesh shader pipeline variants per shader.

Materials are defined in `.fjmat` JSON files that reference a shader, textures, and param values. The properties panel reads shader metadata to render per-uniform inspector controls (color pickers, sliders, drag floats) with live preview. Params are saved back to `.fjmat` on scene save.

### Pipeline Cache

One pipeline set (opaque + transparent + mesh shader) per unique `.fjsl`. All share the same descriptor layout — only the fragment module differs. Pipeline ID 0 is always `standard.fjsl` (empty fragment = standard PBR). Draw commands are sorted by `[pipeline_id:16][material_id:24][mesh_id:24]` and split into per-pipeline draw ranges.

## MegaBuffer

A single large vertex + index buffer holds all loaded geometry. When a model is loaded, its vertices and indices are suballocated into the MegaBuffer and the offsets are recorded. This means all opaque geometry can be drawn without rebinding vertex/index buffers.

## Multi-Draw Indirect

Opaque draws are batched into `VkDrawIndexedIndirectCommand` arrays. The CPU builds the indirect buffer each frame after frustum culling and groups draws by pipeline ID. Each pipeline range gets its own `vkCmdDrawIndexedIndirect` call with the appropriate pipeline bound.

Each draw has a corresponding `GpuDrawData` entry in an SSBO (set 2), containing the model matrix, material ID, and meshlet info. Shaders read per-object data via `gl_DrawID`.

## Mesh Shaders

`VK_EXT_mesh_shader` is supported with a runtime toggle. When enabled, the traditional vertex pipeline is replaced:

- **Task shader** — one workgroup per object. Reads meshlet bounding spheres and normal cones, performs per-meshlet frustum and backface culling, and emits surviving meshlets.
- **Mesh shader** — one workgroup per meshlet. Fetches vertex indices from the meshlet data, transforms vertices, and outputs up to 64 vertices / 124 triangles.
- **Fragment shader** — same PBR fragment shader as the vertex path.

Each `.fjsl` shader gets its own mesh shader pipeline variant (same task + mesh modules, different fragment module). Falls back to the traditional vertex pipeline if the device doesn't support mesh shaders or the user disables them.

## Meshlets

Geometry is clustered into meshlets at load time (max 64 vertices, 124 triangles each). Four SSBOs hold the meshlet data:

| Buffer | Content |
|--------|---------|
| Meshlets | Vertex/triangle offsets and counts per meshlet |
| Bounds | Bounding sphere + normal cone per meshlet (for GPU culling) |
| Vertex indices | Global vertex indices referenced by each meshlet |
| Triangles | Local triangle indices (3 bytes per triangle, packed) |

All meshlet data is bound as descriptor set 3.

## Forward+ Lighting

Point and spot lights are culled per screen-space tile (16x16 pixels) in a compute pass before the color pass. Each tile gets a list of affecting lights. During shading, the fragment shader reads only the lights assigned to its tile.

Light types: directional (global, one), point, and spot. The directional light also drives the cascaded shadow maps.

## Multiple Viewports

Each viewport is an `OffscreenPass` with its own color + depth images and camera. A viewport can display different visualization sources (final color, depth, wireframe, normals, UV, overdraw, bloom, light heatmap, light grid, meshlet viz). Only the passes needed for the active visualization are recorded.

## GPU Particle System

Fully GPU-driven. Three compute dispatches per emitter per frame:

1. **Emit** — spawns new particles from a dead list
2. **Simulate** — updates position/velocity/lifetime, recycles dead particles
3. **Indirect** — fills a `VkDrawIndirectCommand` so the render knows how many quads to draw

Particles render as camera-facing billboard quads with additive or alpha blending.

## Architecture

```
VulkanContext
  ├── Device               Instance, physical/logical device, queues
  ├── Swapchain            Presentation images
  ├── BindlessTextures     Global texture array + material SSBO + custom params SSBO
  ├── MegaBuffer           Shared vertex + index storage
  ├── MeshletBuffer        Meshlet data (4 SSBOs)
  ├── IndirectBatch        MDI command + draw data buffers, per-pipeline draw ranges
  │
  ├── PipelineCache        Per-shader pipeline variants (opaque + transparent + mesh)
  ├── Pipeline             Traditional vertex pipeline (PBR, used by depth/shadow passes)
  ├── MeshShaderPass       Mesh shader color pipeline (legacy, replaced by PipelineCache)
  ├── DepthPrepass         Depth-only (traditional)
  ├── MeshShaderDepthPass  Depth-only (mesh shader)
  ├── ShadowPass           CSM, 3 cascades at 2048x2048
  ├── LightCullPass        Forward+ tile light binning (compute)
  ├── SkyPass              Procedural sky
  ├── TransparentPass      Back-to-front sorted
  ├── ParticleSystem       GPU compute particles
  ├── BloomPass            Downscale chain + composite
  ├── PostPass             shader_type post, blended by post process volumes
  ├── OutlinePass          Selection outline
  ├── DebugDraw            Collider wireframes
  ├── [Debug viz passes]   Depth, normals, UV, overdraw, wireframe,
  │                        meshlet, light heatmap, light grid
  │
  ├── OffscreenPass(es)    Per-viewport color + depth + MSAA resolve
  └── RenderQueue          Sorted draw commands built each frame
```

## Per-Frame Flow

1. CPU frustum cull scene objects
2. Build `IndirectBatch` (MDI commands + draw data SSBO) or mesh shader dispatch list
3. Shadow pass (3 cascades)
4. Depth prepass (traditional or mesh shader, depending on toggle)
5. Light cull compute (Forward+ tile binning)
6. Color pass (PBR with Forward+ lighting, traditional or mesh shader)
7. Sky pass
8. Transparent pass (back-to-front sorted)
9. Particle compute (emit, simulate, indirect) + particle rendering
10. Debug passes (only if the active viewport requests a debug visualization)
11. Bloom (downscale chain + upsample composite)
12. Post (`shader_type post`, per post process volume; see [Post-Process.md](Post-Process.md)),
    then the editor grid and debug lines over its output, ungraded
13. Outline pass (selection highlight)
14. ImGui overlay rendered to swapchain

## Descriptor Sets

| Set | Content |
|-----|---------|
| 0 | Global UBO — view, projection, inverse projection, light direction/color, camera position, ambient, cascade view-projections, cascade splits, tile info |
| 1 | Bindless texture array (up to 1024 textures) + material SSBO (up to 1024 materials) + custom params SSBO (flat float buffer) |
| 2 | `GpuDrawData` SSBO — per-object model matrix, material ID, meshlet offset/count, vertex offset |
| 3 | Meshlet data — meshlets, bounds, vertex indices, triangles (4 SSBOs) |

## Resource Handles

Resources are referenced by generational handles (`Handle<Tag>`) — a 20-bit slot index and a 12-bit generation counter packed into a `uint32_t`. If a resource is freed and the slot reused, old handles won't resolve to the new occupant.

Types: `ModelHandle`, `MaterialHandle`, `TextureHandle`.

`ResourceManager` stores resources in flat vector pools with string-to-handle maps for path-based cache lookups.

## Shader Hot-Reload

Debug builds only. `ShaderWatcher` uses inotify to watch the `shaders/` directory. When a GLSL source file changes, SPIR-V recompilation is triggered and the affected pipeline is destroyed and rebuilt. When a `.fjsl` file changes, the `PipelineCache` re-parses the shader, regenerates the fragment GLSL, recompiles to SPIR-V, and recreates all pipeline variants.
