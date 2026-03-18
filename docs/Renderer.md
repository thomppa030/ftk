# Renderer

## Overview

Fjell uses Vulkan 1.3 with dynamic rendering — no `VkRenderPass` or `VkFramebuffer` objects. All rendering is done through `vkCmdBeginRendering`/`vkCmdEndRendering`, and image layout transitions use `VkImageMemoryBarrier2` (synchronization2).

MSAA is configurable (2x, 4x, or 8x). Multisampled color and depth are resolved before post-processing.

## Bindless Textures

All loaded textures live in a single descriptor array. Materials are stored in an SSBO and reference textures by their array index. Shaders sample via `textures[nonuniformEXT(index)]` and read material properties via `materials[material_id]`.

There are no per-material descriptor sets. Every draw just pushes a `material_id`.

## MegaBuffer

A single large vertex + index buffer holds all loaded geometry. When a model is loaded, its vertices and indices are suballocated into the MegaBuffer and the offsets are recorded. This means all opaque geometry can be drawn without rebinding vertex/index buffers.

## Multi-Draw Indirect

Opaque draws are batched into `VkDrawIndexedIndirectCommand` arrays. The CPU builds the indirect buffer each frame after frustum culling, and the GPU executes all opaque draws in one `vkCmdDrawIndexedIndirect` call.

Each draw has a corresponding `GpuDrawData` entry in an SSBO (set 2), containing the model matrix, material ID, and meshlet info. Shaders read per-object data via `gl_DrawID`.

## Mesh Shaders

`VK_EXT_mesh_shader` is supported with a runtime toggle. When enabled, the traditional vertex pipeline is replaced:

- **Task shader** — one workgroup per object. Reads meshlet bounding spheres and normal cones, performs per-meshlet frustum and backface culling, and emits surviving meshlets.
- **Mesh shader** — one workgroup per meshlet. Fetches vertex indices from the meshlet data, transforms vertices, and outputs up to 64 vertices / 124 triangles.
- **Fragment shader** — same PBR fragment shader as the vertex path.

Falls back to the traditional vertex pipeline if the device doesn't support mesh shaders or the user disables them.

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
  ├── BindlessTextures     Global texture array + material SSBO
  ├── MegaBuffer           Shared vertex + index storage
  ├── MeshletBuffer        Meshlet data (4 SSBOs)
  ├── IndirectBatch        MDI command + draw data buffers
  │
  ├── Pipeline             Traditional vertex pipeline (PBR)
  ├── MeshShaderPass       Mesh shader color pipeline
  ├── DepthPrepass         Depth-only (traditional)
  ├── MeshShaderDepthPass  Depth-only (mesh shader)
  ├── ShadowPass           CSM, 3 cascades at 2048x2048
  ├── LightCullPass        Forward+ tile light binning (compute)
  ├── SkyPass              Procedural sky
  ├── TransparentPass      Back-to-front sorted
  ├── ParticleSystem       GPU compute particles
  ├── BloomPass            Downscale chain + composite
  ├── TonemapPass          AgX tone mapping
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
12. Tone mapping (AgX)
13. Outline pass (selection highlight)
14. ImGui overlay rendered to swapchain

## Descriptor Sets

| Set | Content |
|-----|---------|
| 0 | Global UBO — view, projection, inverse projection, light direction/color, camera position, ambient, cascade view-projections, cascade splits, tile info |
| 1 | Bindless texture array (up to 1024 textures) + material SSBO (up to 1024 materials) |
| 2 | `GpuDrawData` SSBO — per-object model matrix, material ID, meshlet offset/count, vertex offset |
| 3 | Meshlet data — meshlets, bounds, vertex indices, triangles (4 SSBOs) |

## Resource Handles

Resources are referenced by generational handles (`Handle<Tag>`) — a 20-bit slot index and a 12-bit generation counter packed into a `uint32_t`. If a resource is freed and the slot reused, old handles won't resolve to the new occupant.

Types: `ModelHandle`, `MaterialHandle`, `TextureHandle`.

`ResourceManager` stores resources in flat vector pools with string-to-handle maps for path-based cache lookups.

## Shader Hot-Reload

Debug builds only. `ShaderWatcher` uses inotify to watch the `shaders/` directory. When a shader source file changes, SPIR-V recompilation is triggered and the affected pipeline is destroyed and rebuilt.
