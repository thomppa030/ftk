# Renderer

> Stub — fill in.

## Scope
The Vulkan backend: device/swapchain, frame orchestration, bindless resources, descriptor caches, indirect drawing.

## Planned sections
- Layers
  - `GpuCore` — instance, device, queues, swapchain (in `src/renderer/gpu/`)
  - `VulkanContext` — frame orchestration, descriptors, command buffers
  - `RenderSubsystem` — engine-side facade
- Bindless system
  - Texture array, material SSBO, custom params SSBO
  - `reserve_index()` / `update_entry()` lifecycle
  - Descriptor set 0 layout
- Descriptor caches
  - `FrameDescriptorCache` — per-frame allocator
  - `FrameCacheKey`
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
