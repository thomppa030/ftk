# Lighting & GI

> Stub — fill in.

## Scope
How direct lighting, shadows, and indirect lighting fit together. Three GI sources (DDGI, SS-GI, RT reflections) and their tradeoffs.

## Planned sections
- Direct lighting model
  - PBR + GGX
  - Forward+ tile cull
  - Sun / directional, point, spot
- Shadows
  - Cascaded directional (stable cascades, texel snap, PCSS, blending)
  - Point / spot (cube map / 2D)
  - Per-cascade frustum culling
  - Optional static shadow caching (TODO)
- DDGI (Dynamic Diffuse Global Illumination)
  - Probe atlas, irradiance + visibility
  - Probe relocation, classification
  - Multi-bounce
  - Bounds auto-fit
- SS-GI (Screen Space GI)
  - Diffuse trace (depth + color reproject)
  - Emissive pass
  - Composite with DDGI
- Reflections
  - SSR (screen-space)
  - RT reflections (KHR ray tracing) — DDGI-fed indirect on hit
  - Mode toggle in render settings
  - V1 limitations (no skinned in BLAS, no shadow ray, no temporal accum)
- Volumetric fog + godrays
- Sky / sun direction
- Common knobs in `RenderSettings`
