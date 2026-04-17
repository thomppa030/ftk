# Lighting & Post-Processing — Production Readiness Analysis

## Current Capabilities

**Forward+ Tiled Lighting** — full pipeline:
- Cook-Torrance PBR (GGX NDF + Smith G + Fresnel-Schlick)
- 16x16 tile-based light culling via compute, cooperative depth reduction
- Up to 1024 point lights + 1024 spot lights, 256 per tile
- Inverse-square attenuation with radius cutoff, spot angular falloff via smoothstep

**Cascade Shadow Maps** — 3-cascade PCSS:
- 4096x4096 depth atlas (D32), logarithmic-linear splits (lambda 0.75)
- Stable texel-grid snapping to prevent shimmer
- 25-sample rotated Poisson disk with adaptive filter radius
- Cascade blending (10% range), front-face culling, depth clamping
- Skinned mesh shadow variant

**DDGI Global Illumination** — SDF + RT backends:
- 8x4x8 probe grid (256 probes), 128 rays/probe, octahedral irradiance + visibility atlas
- SDF sphere-tracing fallback (128^3 global volume) and ray-traced backend
- Dynamic probe offsets, per-probe status tracking, temporal hysteresis (0.97)
- Half-resolution indirect output, emissive GI contribution (boosted)
- Multi-bounce GI support (infinite_bounces_mult, disabled by default)

**GTAO** — horizon-based AO:
- 4 directions x 4 steps, half-resolution R8 output
- Separable Gaussian blur, interleaved gradient noise
- Modulates ambient + 50% direct lighting

**SSR** — hi-z ray marching:
- DDA-based screen-space trace, 64 max steps, 100m max distance
- Hi-Z mip chain for acceleration
- Samples previous-frame color at hit location

**Volumetric Fog** — froxel-based:
- 4-pass compute: inject, temporal, integrate, composite
- 64 depth slices, exponential distribution, 16x16 tiles
- Henyey-Greenstein phase function, directional shadow sampling
- Height-based exponential density falloff, temporal blend (0.9)
- Disabled by default

**Bloom** — downsample/upsample chain:
- 5 mip levels, bright pixel extraction (threshold 1.0)
- Progressive blur with additive upsample blending

**Tone Mapping** — AgX filmic:
- Troy Sobotka / Blender / Filament implementation
- Full color space pipeline: sRGB -> Rec.2020 -> AgX log2 -> sigmoid -> outset -> gamma

**IBL / Environment** — gradient sky + BRDF LUT:
- 64x64 per-face sky cubemap (7 mips), compute-generated
- 512x512 BRDF split-sum LUT
- Diffuse + specular IBL sampling in mesh.frag

**Emissive** — additive self-illumination:
- Per-material emission_color, emissive texture support
- DDGI integration with emissive_gi_boost

**Debug Visualization** — light heatmap, depth/normal viz, cull/meshlet overlays, DDGI probe preview

---

## What's Missing for Production

### IBL & Environment (the biggest gap)

| Feature | Status | Impact |
|---------|--------|--------|
| **HDR environment maps** | Missing (gradient sky only) | Can't light scenes with real-world environment; indoor/outdoor transitions look flat |
| **Cubemap convolution** | Missing | No roughness-filtered specular from environment; reflections are either sharp or clamped to one mip |
| **Environment probe baking** | Missing | Entire scene uses one global sky cubemap; indoor areas get sky lighting bleeding through |
| **Reflection probes** | Missing | No local specular reflections for interiors, corridors, or enclosed areas |
| **Skybox from texture** | Missing (procedural gradient only) | Can't use HDR panoramas, painted skies, or time-of-day cubemap sets |
| **Atmospheric scattering** | Missing | No Rayleigh/Mie sky model; sunrise/sunset/time-of-day requires manual color picks |

### Exposure & Color

| Feature | Status | Impact |
|---------|--------|--------|
| **Auto-exposure** | Missing (manual only) | Walking from dark interior to bright exterior clips or crushes; every scene needs hand-tuned exposure |
| **Color grading / LUT** | Missing | No artistic color control; shipped scenes all look "raw" |
| **Depth of field** | Missing | No focal depth control for cinematics or gameplay emphasis |
| **White balance** | Missing | Can't compensate for warm/cool light sources |

### Shadow Quality

| Feature | Status | Impact |
|---------|--------|--------|
| ~~**Point/spot light shadows**~~ | Done | 8192x8192 atlas, 1024px tiles, 9-tap Poisson PCF. Spot: single perspective tile. Point: 6-face cubemap (2x3 block). Max 8 spot + 4 point shadowed per frame. |
| **Shadow bias per-material** | Hardcoded (1.25/1.75) | Thin geometry (grass, fences) needs different bias than thick walls; current single bias forces compromise |
| **Static shadow caching** | Missing | Every cascade re-renders every frame even for static geometry; wastes GPU time |
| **Contact shadows** | Missing | Small-scale occlusion near contact points lost at cascade resolution |
| **Transparent shadow receivers** | Missing | Alpha-tested geometry (leaves, chains) either fully blocks or fully passes light |

### GI Quality

| Feature | Status | Impact |
|---------|--------|--------|
| **Emissive GI too weak** | Known issue | Emissive surfaces barely illuminate surroundings despite hitting probes; hysteresis + sparse grid + ray averaging |
| **Auto-placed probe grid** | Missing (manual origin + spacing) | Grid must be hand-positioned per scene; doesn't adapt to level geometry |
| **Per-scene DDGI config** | Missing | One global config; can't have dense probes in a detailed room and sparse probes outdoors |
| **GI intensity packed in cascade_splits.w** | Fragile | Uses padding field in UBO; any cascade refactor breaks GI |
| **Light leaking through thin walls** | Partial (self_shadow_bias) | Bias helps but doesn't fully solve single-sided wall light bleed |

### Lighting Features

| Feature | Status | Impact |
|---------|--------|--------|
| **Area lights** | Missing | No rectangular/disk/tube light sources; all lights are point/spot/directional |
| **IES light profiles** | Missing | Can't shape light distribution for realistic fixtures |
| **Light cookies/projectors** | Missing | No projected textures for patterned shadows (window frames, gobos) |
| **Per-light intensity units** | Arbitrary (color * intensity) | No physical units (lumens/candela); artists guess values per light type |
| ~~**Volumetric lights (point/spot)**~~ | Done | fog_inject.comp loops all point/spot lights with HG phase, windowed attenuation, 4-tap jittered PCF shadow sampling, temporal accumulation |
| **Light groups / layers** | Missing | Can't selectively disable lighting per object or per area |
| **Light materials (.fjsl)** | Missing | Lights can't reference materials to drive color, attenuation curves, or animated properties through the shader system; everything is hardcoded per-light-type |

### Post-Processing

| Feature | Status | Impact |
|---------|--------|--------|
| **Motion vectors** | Missing | TAA, motion blur, and temporal stability of SSR/fog all blocked on this |
| **TAA** | Missing | MSAA misses shader aliasing (specular flickering); no sub-pixel stability |
| **Motion blur** | Missing | Fast camera/object movement looks stroboscopic |
| **Chromatic aberration** | Missing | No lens-style fringe for cinematic look |
| **Film grain** | Missing | No noise overlay for stylistic effect |
| **Vignette** | Missing | No edge darkening for focus |

### Performance & Robustness

| Feature | Status | Impact |
|---------|--------|--------|
| **Light overflow detection** | Missing | Exceeding 1024 point/spot lights or 256/tile silently corrupts rendering |
| **GTAO quality tiers** | Half-res only | No full-res option for quality, no quarter-res for performance |
| **GTAO bilateral blur with normals** | Unknown | May produce halo artifacts at depth edges without cross-bilateral filtering |
| **Volumetric fog quality tuning** | Disabled by default | Ghosting at high temporal blend, single shadow tap (no PCF), no local fog volumes |
| **SSR fallback** | Hard cutoff | Rays that miss return black; no smooth fallback to cubemap reflections |
| **Resolution-hardcoded shader math** | Half-res GTAO/DDGI baked into mesh.frag | Changing AO or GI resolution breaks UV calculations |
| **VRS light-density-driven** | Missing | Forward+ tile light counts available but not driving shading rate |

---

## Priority Ranking for Production

**Tier 1 — Can't ship without:**
1. ~~Point/spot light shadows~~ — done. LocalShadowPass with 8192x8192 atlas, spot + point cubemap rendering, 9-tap rotated Poisson PCF, spot direction follows world transform
2. ~~Auto-exposure (luminance histogram)~~ — done. Manual EV slider + 256-bin log2 histogram compute pass + temporal adaptation. Device-local SSBO, disabled by default, enabled via auto-exposure toggle.
3. ~~HDR environment maps~~ — done. Loads .hdr equirectangular via stbi_loadf, converts to 512x512 cubemap, GGX-prefiltered mip chain for split-sum specular IBL. WorldEnvironmentNode hdri_path field, cached, serialized.
4. ~~Cubemap convolution for specular IBL~~ — done (part of #3). Prefilter compute shader importance-samples GGX NDF per mip level. IBL sampling uses `roughness * 9.0` for 10-mip chain.
5. ~~Fix emissive GI weakness~~ — done. DDGI accel structures were silently failing (missing queue/mega_buffer in pre_record context), and light direction was hardcoded. Fixed in data-driven renderer session 2026-04-13.
6. ~~Light overflow detection + graceful clamping~~ — done. Warns when point/spot count exceeds 1024 buffer limit. Per-tile clamping at 256 was already handled in shader.
7. ~~Motion vectors + TAA~~ — done. Velocity attachment in offscreen pass, TaaResolvePass with history buffer, unjittered VP matrices in GlobalUbo for reprojection.

**Tier 2 — Looks amateur without:**
8. Contact shadows — screen-space ray march for small-scale occlusion that cascades miss
9. Color grading / 3D LUT — artists need post-process color control to establish visual tone
10. ~~SSR cubemap fallback~~ — reverted. Additive composite double-counted environment reflections with IBL, causing visible patches on metallic surfaces. IBL specular already provides the environment fallback.
11. Reflection probes — indoor scenes need local specular; the global sky cubemap is wrong for enclosed spaces
12. Static shadow caching — re-rendering 3 full cascade passes of static geometry every frame is expensive for no visual gain
13. Depth of field — gameplay and cinematics need focal separation
14. ~~GTAO bilateral blur with normals~~ — done (thickness rejection). Samples further than thickness parameter are rejected as non-occluders. Fixes dark halos at depth discontinuities and mesh joints.
15. ~~Decouple GI intensity from cascade_splits.w~~ — done. Dedicated `gi_intensity` float in GlobalUbo, read directly in shaders.
16. Per-scene DDGI config — different areas need different probe density; one global config doesn't scale
17. ~~Volumetric point/spot lights~~ — done. Windowed attenuation, per-froxel jittered PCF, temporal accumulation

**Tier 3 — Nice to have:**
18. Atmospheric scattering (Rayleigh/Mie sky model) — procedural time-of-day without manual gradient painting
19. Area lights (rectangular/disk/tube) — realistic light shapes for architectural and interior scenes
20. Light cookies/projectors — textured light falloff for windows, gobos, patterned shadows
21. VRS with forward+ light density — coarse shading in low-light tiles for free performance
22. Local fog volumes — per-area fog shapes instead of global height fog only
23. ~~Volumetric fog PCF shadow sampling~~ — done. 4-tap jittered PCF for local lights, single-tap for directional (froxel resolution makes more taps pointless)
24. IES light profiles — photometric light distribution for fixture accuracy
25. GTAO quality tiers (quarter/half/full res) — scalability knob for different hardware
26. Motion blur — per-object velocity-based blur once motion vectors exist
27. Film grain / chromatic aberration / vignette — cinematic post-process stack
28. White balance — temperature/tint correction in tone mapping
29. Alpha-tested shadow receivers — leaves and fences need proper shadow behavior
30. Light materials — extend lights with `.fjsl` material references for custom attenuation, color, animation

---

## Future: Light Materials

Lights currently have fixed properties (color, intensity, radius, angles). A light material system would let lights reference `.fjsl` shaders to customize their behavior, reusing the existing material infrastructure.

### What It Enables

- **Custom attenuation curves** — physically-based falloff, stylized falloff, or game-specific curves that aren't inverse-square
- **Animated lights** — flickering torches, pulsing alarms, lightning flashes driven by `TIME` in a shader rather than per-light script code
- **Color temperature** — Kelvin-based color computed in the shader, consistent across all lights of a type
- **IES profiles via shader** — encode photometric distribution as a lookup function rather than requiring a separate texture system
- **Light cookies as materials** — project a texture pattern through the light's cone, combining cookie and shadow in one system
- **Consistent authoring** — artists already know `.fjsl`; extending it to lights means one authoring model for surfaces and lights

### Possible Approach

A `light()` function in `.fjsl` analogous to `fragment()` for surfaces:

```glsl
shader_type light;
uniform float flicker_speed = 8.0;
uniform float flicker_intensity = 0.3;
uniform vec3 base_color : hint_color = vec3(1.0, 0.6, 0.2);

void light() {
    float flicker = 1.0 - flicker_intensity * (0.5 + 0.5 * sin(TIME * flicker_speed));
    LIGHT_COLOR = base_color;
    LIGHT_INTENSITY = LIGHT_INTENSITY * flicker;
}
```

Built-in inputs: `TIME`, `LIGHT_POSITION`, `LIGHT_DIRECTION`, `LIGHT_RADIUS`, `FRAG_DISTANCE`, `FRAG_DIRECTION`.
Built-in outputs: `LIGHT_COLOR`, `LIGHT_INTENSITY`, `LIGHT_ATTENUATION`.

The light material would be evaluated per-fragment in `accumulate_tile_lights()` before the PBR BRDF, replacing the hardcoded `color * intensity * attenuation` with shader-driven values. `.fjmat` files would bind to light nodes the same way they bind to mesh nodes.

### Prerequisites

- Light cookies/projectors (provides the texture sampling infrastructure)
- IES profiles (provides the distribution function infrastructure)
- Neither is strictly required — light materials could start with just color/intensity/attenuation overrides and grow to subsume cookies and IES later
