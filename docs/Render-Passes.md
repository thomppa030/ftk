# Render Passes

## Current Pipeline

| Order | Pass | Status | Description |
|-------|------|--------|-------------|
| 0 | Shadow map (CSM) | done | 3-cascade depth from directional light, PCF filtered |
| 1 | Depth pre-pass | done | Depth-only with minimal vertex shader, eliminates overdraw |
| 2 | Forward shading (PBR) | done | Cook-Torrance BRDF, depth equal / no depth write |
| 3 | Debug wireframes | done | Collider visualization, line-list pipeline |
| 4 | Depth visualization | done | Fullscreen linearization pass for viewport debug |

## Planned (in order)

| Pass | Priority | Description |
|------|----------|-------------|
| Bloom | next | Downscale chain (CoD-style), blur, composite back |
| Tone mapping extraction | high | Pull Reinhard out of mesh.frag, replace with AgX |
| Transparent pass | high | Back-to-front sorted, required before VFX/particles |
| Sky pass | medium | Skybox or procedural sky at infinite depth after opaques |
| GTAO | medium | Ground truth ambient occlusion from depth + normals |
| TAA | medium | Temporal anti-aliasing with motion vectors |

## All Possible Passes

### Geometry

| Pass | Notes |
|------|-------|
| Depth pre-pass | **Done.** Writes depth buffer with cheap shader |
| G-buffer pass | Only needed for deferred shading (not planned) |
| Visibility buffer | Cutting-edge deferred texturing (not planned) |

### Shadows

| Pass | Notes |
|------|-------|
| Shadow map (CSM) | **Done.** 3 cascades, 2048x2048 each |
| Point/spot light shadow maps | One cube/2D depth map per light, when point/spot lights are added |
| Screen-space contact shadows | Short-range ray march for fine detail shadow maps miss |

### Scene Rendering

| Pass | Notes |
|------|-------|
| Forward shading (PBR) | **Done.** Cook-Torrance, depth equal after pre-pass |
| Transparent pass | Back-to-front sorted, separate from opaques. Required before VFX |
| Debug wireframes | **Done.** Collider overlays |
| Sky / environment pass | Skybox or procedural sky, drawn at infinite depth after opaques |

### Screen-Space Effects

| Pass | Notes |
|------|-------|
| SSAO / GTAO | Ambient occlusion from depth + normals. GTAO is the modern variant |
| SSR | Screen-space reflections via depth buffer ray march |
| SSGI | Screen-space global illumination — bounce lighting approximation |

### Post-Processing (in pipeline order)

| Pass | Notes |
|------|-------|
| Bloom | Downscale chain with blur, composite. Makes emissives and bright lights glow |
| Tone mapping | Convert HDR to LDR. Currently Reinhard baked in mesh.frag — extract to own pass, use AgX |
| Auto exposure / eye adaptation | Compute average luminance, adjust exposure dynamically |
| Color grading / LUT | 3D lookup table for stylistic color adjustment |
| Depth of field | Blur based on distance from focal plane, circle-of-confusion from depth |
| Motion blur | Blur along velocity vectors (needs velocity buffer from TAA) |
| FXAA | Cheap single-pass edge anti-aliasing |
| TAA | Temporal accumulation with motion vectors, better quality than FXAA |
| Film grain | Noise overlay, usually combined with other trivial effects |
| Chromatic aberration | RGB channel offset at screen edges |
| Vignette | Darken edges of the frame |
| Sharpening | CAS or similar, counteracts TAA softening |

### Utility / Debug

| Pass | Notes |
|------|-------|
| Depth visualization | **Done.** Linearized depth buffer in viewport |
| Shadow map visualization | Display cascade layers as viewport source |
| Normals visualization | Display world-space normals as colors (if G-buffer exists) |
| Wireframe overlay | Already covered by debug wireframe pass |
| Light heatmap | Visualize light count per tile (when forward+ lands) |

### Compute

| Pass | Notes |
|------|-------|
| Light culling | Tile/cluster binning for forward+. Needed with 10+ lights |
| Hi-Z generation | Hierarchical depth mip chain for GPU occlusion culling |
| Frustum culling (GPU) | Compute pass that culls draw commands before rendering |
| Particle simulation | GPU-driven particle update for VFX system |
