# FJSL — Spatial Shaders

> Stub — fill in.

## Scope
The Godot-inspired shader format. Author a `fragment()` (and optionally `vertex()`) that extends the engine's PBR pipeline by writing to built-in outputs. Empty `fragment()` = standard PBR.

Other shader types share the format: `shader_type sky;` for the sky pass ([Sky Shaders](../Environment/Sky-Shaders.md)), `shader_type ui;` for game UI quads ([Game-UI/Materials](../Game-UI/Materials.md)), `shader_type terrain;` and `shader_type water;` for those surfaces, and `shader_type fog;` for the volumetric fog ([Fog Shaders](../Environment/Fog-Shaders.md)).

## Planned sections
- File anatomy
  - `shader_type spatial;`
  - `render_mode unlit;` and other modes
  - `uniform` declarations with hints
- Built-in fragment inputs
  - `UV`, `WORLD_POS`, `WORLD_NORMAL`, `TBN`, `VIEW_DIR`, `COLOR`, `TIME`
- Built-in outputs
  - `ALBEDO`, `ROUGHNESS`, `METALLIC`, `NORMAL`, `EMISSION`, `AO`, `ALPHA`
- Uniform hints (drive inspector UI)
  - `hint_color`, `hint_range(min, max, step)`, `hint_black`, `hint_normal`, asset hints
- The vertex stage — `vertex()` for displacement (meshlet-compatible)
- Texture sampling — `uniform sampler2D` + `texture(...)`
- Compilation pipeline — `.fjsl` → GLSL → SPIR-V via `glslc`
- Per-shader pipeline variants — opaque / transparent / mesh-shader / shadow
- Hot-reload behavior
- Worked examples
  - Tint
  - Animated UV scroll
  - Triplanar
  - Vertex wave displacement
