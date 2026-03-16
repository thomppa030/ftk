# Renderer

## Vulkan 1.3 Dynamic Rendering

The renderer uses Vulkan 1.3 core features — no `VkRenderPass` or `VkFramebuffer` objects. Rendering is done with `vkCmdBeginRendering`/`vkCmdEndRendering` and image layout transitions are handled explicitly via `VkImageMemoryBarrier2` (synchronization2).

## Architecture

```
VulkanContext
  ├── Device                 VkInstance, VkPhysicalDevice, VkDevice, queues
  ├── Swapchain              Swapchain images, views, depth buffer
  ├── OffscreenPass          Color + depth images for viewport rendering
  ├── Pipeline               PBR graphics pipeline (mesh.vert/mesh.frag)
  ├── DebugDraw              Collider wireframe line-list pipeline
  ├── Descriptors
  │     ├── Set 0: Global    View, projection, lighting (uniform buffer)
  │     └── Set 1: Material  4 texture samplers (base color, normal, metallic/roughness, emissive)
  ├── Command Buffers        2 (double-buffered)
  └── Sync Objects           Semaphores + fences for frame pacing
```

## Per-Frame Command Buffer

Two rendering passes per frame, both using dynamic rendering:

### Pass 1: Scene (offscreen)

Renders to `OffscreenPass` color/depth images.

```
1. Barrier: color image  UNDEFINED → COLOR_ATTACHMENT_OPTIMAL
2. Barrier: depth image  UNDEFINED → DEPTH_ATTACHMENT_OPTIMAL
3. vkCmdBeginRendering (color + depth attachments, clear both)
4. Bind PBR pipeline
5. Set viewport/scissor (dynamic state)
6. Bind global descriptor set (set 0)
7. For each RenderObject:
     Bind material descriptor set (set 1)
     Push PbrPushConstants (model matrix + PBR params)
     Draw mesh
8. Optional: bind debug pipeline, draw collider wireframes
9. vkCmdEndRendering
10. Barrier: color image  COLOR_ATTACHMENT_OPTIMAL → SHADER_READ_ONLY_OPTIMAL
```

### Pass 2: UI (swapchain)

Renders ImGui to the swapchain image. The offscreen texture is displayed as an `ImGui::Image` in the viewport panel.

```
1. Barrier: swapchain image  UNDEFINED → COLOR_ATTACHMENT_OPTIMAL
2. vkCmdBeginRendering (color only, clear to background)
3. ImGui render (overlay callback)
4. vkCmdEndRendering
5. Barrier: swapchain image  COLOR_ATTACHMENT_OPTIMAL → PRESENT_SRC_KHR
```

## Pipeline Configuration

### PBR Pipeline

- **Topology:** Triangle list
- **Vertex format:** position (vec3), color (vec3), texcoord (vec2), normal (vec3), tangent (vec4)
- **Dynamic state:** Viewport, scissor
- **Depth:** Test enabled, write enabled, compare less
- **Cull mode:** None
- **Push constants:** `PbrPushConstants` (96 bytes) — model matrix, base color factor, metallic/roughness factors, normal map flag

### Debug Pipeline

- **Topology:** Line list
- **Vertex format:** position (vec3) only
- **Depth:** Test enabled, write *disabled* (lines occluded by scene but don't occlude it)
- **Push constants:** `DebugPushConstants` (80 bytes) — model matrix, color

## Shaders

### mesh.vert / mesh.frag

Full PBR (Cook-Torrance BRDF):
- GGX/Trowbridge-Reitz normal distribution
- Schlick-GGX geometry function (Smith's method)
- Fresnel-Schlick approximation
- Normal mapping via TBN matrix
- Reinhard tone mapping

Uniform data:
- **Set 0, binding 0:** GlobalUbo (view, projection, light direction/color/intensity, camera position, ambient)
- **Set 1, bindings 0-3:** base_color_map, normal_map, metallic_roughness_map, emissive_map

### debug.vert / debug.frag

Minimal pass-through. Transforms position via GlobalUbo view/projection + push constant model matrix. Outputs a solid color from push constants.

## Resource Management

`ResourceManager` caches GPU resources to avoid redundant uploads:

```
load_model(path)      → Model (meshes + textures from glTF)
load_texture(path)    → Texture (VkImage + view + sampler)
create_material(name) → Material (descriptor set binding textures)
create_primitive(type) → Model (cube/plane/sphere generated on CPU)
```

Default textures (1x1 white, flat normal, etc.) are created at startup for materials that don't specify all maps.

## Shader Hot-Reload (Debug)

`ShaderWatcher` uses inotify to watch the `shaders/` directory. When a `.vert` or `.frag` changes, the external `glslc` compilation runs (via the build system), and `Pipeline::recreate()` destroys and rebuilds the graphics pipeline with the new SPIR-V.
