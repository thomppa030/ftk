# Render Passes

> Stub — fill in.

## Scope
A reference of every pass under `src/renderer/pass/` — what it does, what it reads, what it writes, where it sits in the frame. Grouped by topic.

## Planned sections (group → passes)

### Visibility & culling
- `instance_cull_pass` — coarse object cull
- `meshlet_instance_pass` — populate meshlet instances
- `meshlet_cull_pass2` — two-pass meshlet cull (frustum + Hi-Z + cone)
- `light_cull_pass` — forward+ tile cull
- `hiz_pass` — Hi-Z pyramid build

### Depth & prepass
- `depth_prepass`
- `mesh_shader_depth_pass`
- `second_cull_depth_pass` — adds what the second meshlet cull found to the depth
- `depth_resolve_pass`
- `normal_pass`
- `contact_shadow_pass`

### Shadows
- `LocalShadowPass` (spot/point)
- Cascade shadow passes — per-cascade frustum, stable cascades, PCSS

### Lighting
- `opaque_pass` — main forward+ shading
- `gtao_pass` — ground-truth AO
- `ddgi_pass` — DDGI probe update + sample
- SS-GI passes — diffuse, specular, composite
- `ssr_trace_pass`, `ssr_composite_pass`
- `rt_reflection_pass` (when ray tracing available)

### Transparency & VFX
- Particle passes (lit, opaque, transparent, mesh particles, trails)
- `distortion_pass`, `distortion_apply_pass`

### Post
- `bloom_pass`
- `exposure_pass` (auto-exposure pipeline)
- Volumetric fog
- TAA (motion vectors)
- `post_pass` (`shader_type post`, blended by post process volumes)

### Debug / dev
- `debug_pass` — debug draw queue, over the post output
- `meshlet_viz_pass`, `light_viz_pass`, `depth_viz_pass`
- `grid_pass` — editor grid, over the post output

Each entry will list: inputs, outputs, conditional toggles, perf rough numbers.
