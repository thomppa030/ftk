# Materials

Fjell uses a Godot-style spatial shader system. Lit materials have built-in PBR slots, and optional `.fjsl` code extends the pipeline by writing to outputs like `ALBEDO`, `ROUGHNESS`, etc. The engine handles all lighting.

## .fjsl Shaders

A `.fjsl` file declares a shader type, uniforms, and a `fragment()` body:

```glsl
shader_type spatial;

uniform sampler2D noise_tex : hint_black;
uniform vec3 tint : hint_color = vec3(1.0, 0.5, 0.2);
uniform float noise_strength : hint_range(0.0, 1.0, 0.01) = 0.5;

void fragment() {
    ALBEDO = texture(noise_tex, UV).rgb * tint;
    ROUGHNESS = 0.8;
}
```

An empty `fragment()` produces standard PBR — that's what `standard.fjsl` does.

Every hook (`fragment()`, `vertex()`, `light()`, and the other shader types' `layers()`, `sky()`, `color()`, `density()`) is a function of its own, as in Godot: `return` ends the hook and keeps what it has set, and the engine carries on from there.

```glsl
void fragment() {
    if (UV.x > 0.9) {
        ALBEDO = vec3(0.01);  // a dark border, lit like the rest
        return;
    }
    ALBEDO = texture(noise_tex, UV).rgb * tint;
}
```

### Built-in Outputs

All have PBR defaults. Just write the ones you want to change.

| Output | Type | Default |
|--------|------|---------|
| `ALBEDO` | vec3 | Base color texture * factor * vertex color |
| `ALPHA` | float | Base color alpha |
| `METALLIC` | float | MR texture blue * factor |
| `ROUGHNESS` | float | MR texture green * factor |
| `NORMAL` | vec3 | Normal map or interpolated normal |
| `EMISSION` | vec3 | Emissive texture |
| `AO` | float | 1.0 (combined with screen-space GTAO) |

### Built-in Inputs

| Input | Type | What it is |
|-------|------|------------|
| `UV` | vec2 | Texture coordinates |
| `WORLD_POS` | vec3 | Fragment world position |
| `WORLD_NORMAL` | vec3 | Interpolated world normal |
| `TBN` | mat3 | Tangent-bitangent-normal matrix |
| `VIEW_DIR` | vec3 | Normalized camera direction |
| `COLOR` | vec3 | Vertex color |
| `BASE_COLOR_TEX` | sampler2D | PBR base color texture |
| `NORMAL_TEX` | sampler2D | PBR normal map |
| `METALLIC_ROUGHNESS_TEX` | sampler2D | PBR metallic-roughness texture |
| `EMISSIVE_TEX` | sampler2D | PBR emissive texture |
| `BASE_COLOR_FACTOR` | vec4 | Material base color factor |
| `METALLIC_FACTOR` | float | Material metallic factor |
| `ROUGHNESS_FACTOR` | float | Material roughness factor |
| `HAS_NORMAL_MAP` | float | 1.0 if a normal map is bound |

### Uniform Hints

Hints control inspector UI and default textures for samplers.

| Hint | Effect |
|------|--------|
| `hint_color` | ColorEdit3/4 in inspector |
| `hint_range(min, max, step)` | SliderFloat in inspector |
| `hint_enum("A,B,C")` | Drop-down of named values (int) |
| `hint_lut` | A colour grading table from a `.fjlut` (sampler2D); the neutral table when unset. Sample it with `lut_sample()` in a post shader |
| `hint_white` | Default to white texture (sampler2D) |
| `hint_black` | Default to black texture (sampler2D) |
| `hint_normal` | Default to flat normal texture (sampler2D) |
| `hint_default_mr` | Default to metallic-roughness texture (sampler2D) |

Uniforms are `float`, `vec2`, `vec3`, `vec4`, `int` or `sampler2D`. An `int`
takes one float of the material's parameters like a `float` does, so a track
or a blend moves it as a number, and the shader reads it rounded to the
nearest whole one. Named with `hint_enum`, it is a choice: the `.fjmat` stores
the index and the inspector shows the names.

```glsl
uniform int falloff : hint_enum("Linear,Smooth,Sharp") = 1;
```

## .fjmat Files

A `.fjmat` file binds a shader to textures and parameter values:

```json
{
    "shader": "shaders/tint.fjsl",
    "textures": {
        "base_color": "assets/textures/brick.png",
        "noise_tex": "assets/textures/noise.png"
    },
    "params": {
        "tint": [0.8, 0.6, 0.4],
        "noise_strength": 0.3
    }
}
```

- `shader` — path to the `.fjsl` file
- `textures` — maps uniform names (or PBR slot names like `base_color`, `normal_map`, `metallic_roughness`, `emissive`) to texture paths
- `params` — maps uniform names to values. Scalars are plain numbers, vectors are arrays. Params not listed use the shader's declared defaults.

### Using in a Scene

Set the object's `material` field to the `.fjmat` path in the scene file:

```json
{
    "name": "Brick Wall",
    "source": "primitive:cube",
    "material": "assets/materials/tinted_brick.fjmat",
    "position": [0, 0, 0]
}
```

## How It Works

1. **Parse** — the FJSL parser reads the `.fjsl` file and extracts uniform declarations, hints, defaults, and the fragment body
2. **Generate** — a complete GLSL fragment shader is produced from `fjell_spatial_template.glsl` with param reads substituted in and the body placed in the template's hook function, which `main()` calls between filling the built-ins and shading from them
3. **Compile** — the `ShaderCompiler` compiles the generated `.frag` to SPIR-V with `glslc` at runtime, or copies it from `shaders/generated/.fjcache` when the same source was compiled before
4. **Pipeline** — the `PipelineCache` creates opaque, transparent, and mesh shader pipeline variants using the compiled fragment module
5. **Render** — draw commands are sorted by pipeline ID, and each range gets its pipeline bound before the indirect draw call

## Hot-Reload

Edit a `.fjsl` file while the engine is running. The shader watcher detects the change, re-parses, regenerates, recompiles, and swaps all pipeline variants live.

## Inspector

When an object uses a `.fjmat` material, the Properties panel shows a Material section with the shader path and per-uniform controls generated from the shader metadata. Changes write directly to the GPU params buffer for instant feedback and are saved back to the `.fjmat` file on scene save.

## Texture Slots

- `texture_indices[0-3]` — fixed PBR (base_color, normal, metallic_roughness, emissive)
- `texture_indices[4-15]` — custom shader textures, twelve shared by a shader and every `shader_function` composed into it

Each is an index into the bindless table, naming a texture and the sampler it
is read with. A `sampler2D` uniform, and `BASE_COLOR_TEX` and the other
built-in textures, is that texture as a handle: `texture`, `textureLod`,
`textureGrad`, `textureGather`, `texelFetch`, `textureSize` and
`textureQueryLevels` read it as they would any sampler, and a function of your
own takes it as a `sampler2D` parameter:

```glsl
uniform sampler2D ramp : hint_black;

vec3 ramp_at(sampler2D t, float x) {
    return texture(t, vec2(x, 0.5)).rgb;
}
```

Give a float where a built-in takes one: `textureLod(SCREEN_TEXTURE, uv, 0.0)`.
The handle's reads are declared beside GLSL's own, and in a shader with both
a whole number there (`0`) fails to compile with "no matching overloaded
function found", whatever the sampler.
