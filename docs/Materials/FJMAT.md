# FJMAT — Material Assets

> Stub — fill in.

## Scope
The JSON material asset: binds a `.fjsl` shader to specific textures and parameter values. One shader, many materials.

## Planned sections
- File anatomy
  ```json
  {
    "shader": "shaders/tint.fjsl",
    "textures": { "base_color": "assets/textures/brick.png" },
    "params": { "tint": [0.8, 0.6, 0.4], "strength": 0.3 }
  }
  ```
- Texture references
  - File path (`assets/textures/foo.png`)
  - `$viewport:<name>` for render-to-texture targets (see [Viewports](../Viewports.md))
- Param storage — flat SSBO, written from `.fjmat`
- Editor flow — material inspector reads FJSL hints, writes back on save
- Hot-reload: changing `.fjmat` re-binds without recompiling the shader
- Common pitfalls — texture format mismatches, missing samplers, bindless index reuse
