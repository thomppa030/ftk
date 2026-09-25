# Render Passes

## Active Pipeline

These run every frame (in order), though some are skipped depending on the active viewport source and scene state.

| Order | Pass | Description |
|-------|------|-------------|
| 1 | Shadow map (CSM) | 3-cascade depth rendering from the directional light, 2048x2048 per cascade, PCF filtered in the color pass |
| 2 | Depth prepass | Depth-only with minimal shader. Traditional vertex or mesh shader path depending on runtime toggle |
| 3 | Light cull (compute) | Forward+ tile-based light binning. Divides the screen into 16x16 tiles, tests each point/spot light against tile frustums, writes per-tile light lists |
| 4 | Forward shading (PBR) | Cook-Torrance BRDF. Reads per-tile light lists from Forward+. Depth-equal test against prepass, no depth write. Traditional vertex or mesh shader path |
| 5 | Sky | Procedural sky, rendered at infinite depth after opaques |
| 6 | Transparent pass | Back-to-front sorted, separate from opaques |
| 7 | Particles | GPU compute emit/simulate/indirect, then billboard quad rendering (additive or alpha blend) |
| 8 | Debug passes | Only recorded if the viewport requests a debug visualization (see below) |
| 9 | Bloom | CoD-style progressive downscale chain, then upsample composite back to full resolution |
| 10 | Post | The scene colour to what the viewport shows through `shader_type post` shaders, blended per camera by the post process volumes; see [Post-Process.md](Post-Process.md) |
| 11 | Game UI | .fjui/.fjss overlay — SSBO quads with SDF rounded rects, no depth, alpha blended |
| 12 | Outline | Selection outline for hovered/selected objects |
| 13 | MSAA resolve | Resolves multisampled color and depth to single-sample targets for post-process and display |
| 14 | ImGui | UI overlay rendered directly to the swapchain image |

## Debug Visualizations

Each viewport can independently switch its visualization source. These passes only run when requested.

| Visualization | Description |
|---------------|-------------|
| Depth | Linearized depth buffer displayed as grayscale |
| Wireframe | Scene geometry as wireframe overlay |
| Normals | World-space normals mapped to RGB |
| UV | Texture coordinates visualized as RG color |
| Overdraw | Per-pixel draw count as a heat gradient |
| Meshlet viz | Each meshlet rendered in a distinct color |
| Light heatmap | Per-tile light count as a heat gradient (Forward+ debugging) |
| Light grid | Tile grid overlay showing light assignment boundaries |

## Mesh Shader Passes

The mesh shader pipeline mirrors the traditional vertex pipeline for two stages:

| Pass | Traditional | Mesh shader |
|------|-------------|-------------|
| Depth prepass | `DepthPrepass` (depth.vert) | `MeshShaderDepthPass` (depth_mesh.task + depth_mesh.mesh) |
| Color pass | `Pipeline` (mesh.vert + mesh.frag) | `MeshShaderPass` (mesh.task + mesh.mesh + mesh.frag) |

Both paths produce identical results. The task shader performs per-meshlet frustum and backface culling before dispatching surviving meshlets to the mesh shader.

Switching between paths is a runtime toggle — no restart needed.

## Planned / Future

| Pass | Notes |
|------|-------|
| GTAO | Ground truth ambient occlusion from depth + normals |
| TAA | Temporal anti-aliasing with motion vectors |
| SSR | Screen-space reflections via depth buffer ray march |
| Point/spot shadow maps | Per-light depth maps (cube map for point, 2D for spot) |
| Auto exposure | Compute average luminance, adjust exposure dynamically |
| Depth of field | Blur based on distance from focal plane |
| Motion blur | Per-pixel blur along velocity vectors |
