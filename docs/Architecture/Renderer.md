# Renderer

> Stub — fill in.

## Scope
The renderer on the [GPU interface](GPU-Interface.md): frame orchestration, bindless resources, indirect drawing, passes.

## Planned sections
- Layers
  - `gpu::Device` — the GPU interface's device, with its Vulkan backend in `ftk/src/ftk/gpu/vulkan/`
  - `GpuContext` — the device, the main swapchain and the engine's shared GPU resources
  - `VulkanContext` — frame orchestration and the views drawn each frame
  - `RenderSubsystem` — engine-side facade
- Bindless system
  - Texture array, material SSBO, custom params SSBO
  - `reserve_index()` / `update_entry()` lifecycle
  - Descriptor set 0 layout
- Shared layouts the passes bind (globals, bindless, draw data, meshlets); binding itself is the [GPU interface](GPU-Interface.md)'s
- Indirect drawing
  - `IndirectBatch` — sorted by pipeline, multi-draw indirect
  - Per-pipeline draw ranges
- Meshlet path
  - `.fjmesh` meshlets, cone culling, frustum + Hi-Z
  - Two-pass occlusion culling
- Vertex shader fallback path
- Pipeline cache (`pipeline_cache`)
- Render stats (`render_stats`)
- Where the framegraph fits — points at [Framegraph](Framegraph.md)
