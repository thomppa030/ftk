# UI Materials — `shader_type ui`

A UI node can be painted by a shader instead of a flat colour, gradient or
texture: `material="name"` on the node points at a `.fjmat` whose `.fjsl` is
`shader_type ui;`. The shader runs once per quad through its own pipeline, and
everything else about the node stays a stylesheet matter: size, corners, border,
clipping and opacity are applied after the shader, so it only paints the fill.

```html
<bar class="vital-fill" value="{value}" max="{max}" material="ui_health_bar"/>
```

```json
{
    "shader": "shaders/ui_health_bar.fjsl",
    "params": { "caustic_intensity": 0.25, "caustic_color": [0.85, 1.0, 0.73] }
}
```

`material=` accepts a name from `assets/materials/<name>.fjmat`, a project-relative
`.fjmat` path, or a `{prop}` template inside a widget. A spatial material on a UI
node is an error: it is reported with the markup file and line in the console and
the UI editor's Problems list, and the node draws its plain colour.

## Writing a ui shader

```glsl
shader_type ui;

uniform float speed : hint_range(0.0, 3.0, 0.1) = 1.0;
uniform vec3 glow : hint_color = vec3(1.0, 0.3, 0.2);
uniform sampler2D noise;

void fragment() {
    float wave = sin(UV.x * 12.0 - TIME * speed) * 0.02;
    float surface = FILL + wave;
    float inside = step(1.0 - surface, UV.y);
    ALBEDO = mix(COLOR.rgb, glow, 0.3 + 0.2 * HOVER);
    ALPHA = COLOR.a * inside;
}
```

Uniforms, hints, `hint_color` colour space and custom `sampler2D` slots work
exactly as in a spatial shader, and the material inspector edits them live. There
is no `vertex()` and no `light()`; declaring either is an error. `render_mode` has
no effect: a UI quad always alpha-blends.

### Inputs

| Name | Type | Meaning |
|---|---|---|
| `UV` | vec2 | 0..1 across the quad, y down |
| `SIZE` | vec2 | the quad's size in layout pixels |
| `PIXEL` | vec2 | `UV * SIZE`, for pixel-exact patterns |
| `SCREEN_UV` | vec2 | position in the UI's design frame, for effects anchored to the screen |
| `TIME` | float | seconds, running in play mode and in the UI editor preview |
| `COLOR` | vec4 | the node's stylesheet colour, linear, with its alpha |
| `TEXTURE` | sampler2D | the node's `src` texture; white when there is none |
| `HAS_TEXTURE` | bool | whether `src` was set |
| `FILL` | float | the fill a `<bar>` shows, 0..1, which springs after its value; 1 on any other element |
| `FILL_TARGET` | float | where the bar's value is right now |
| `FILL_VELOCITY` | float | how fast `FILL` is moving, in bar widths per second, negative while draining |
| `FILL_AGITATION` | float | 0..1, stirred up by a sudden change and dying away over about half a second; a drop stirs harder than a rise |
| `HOVER`, `ACTIVE`, `FOCUS` | float | 1 while the pseudo-class holds, else 0 |

### Outputs

`ALBEDO` (vec3) and `ALPHA` (float), seeded with `COLOR.rgb` and `COLOR.a`. The
template then draws the border, cuts the rounded corners, applies `opacity` and the
clip rect, and discards fully transparent fragments.

## Bars

A `<bar>` without a material shows its fill by shrinking its quad. With a material
the quad stays the full track and the shader reads `FILL`, so it decides the edge:
a straight cut with `step(UV.x, FILL)`, a wave, a gradient into the empty part, or
nothing at all for a shader that ignores the fill.

The fill a bar shows is not the raw value: it follows the value like a mass on a
spring, tuned by the bar's `settle` and `bounce` attributes, and a sudden change
leaves an agitation behind that decays on its own. A liquid shader uses
`FILL_AGITATION` to slosh after a hit and `FILL_VELOCITY` to send its waves the way
the liquid moves; `shaders/ui_liquid_fill.fjsl` does both.

## Where the texture goes

A ui shader ignores the `base_color`, `normal_map`, `metallic_roughness` and
`emissive` slots of the `.fjmat`; the node's own `src` arrives as `TEXTURE`, and any
extra image is a `uniform sampler2D` bound in the `.fjmat` like a spatial shader's.
`$viewport:name` works there too, so a minimap or portal is a ui material sampling a
scene viewport.

## Hot reload and the editor

Editing the `.fjsl` on disk recompiles it and swaps the pipeline on the next frame,
in play mode and in the UI editor. A compile error keeps the last working pipeline
and prints the glslc message. Params edited in the inspector apply immediately.

The material editor and the content browser cannot render a ui material on their
preview sphere; assign it to a node and open that widget in the UI editor. Engine
shaders live in `shaders/`, which is the directory the shader watcher follows.

Worldspace widgets run the same shader through a second pipeline that writes the
scene pass's velocity attachment, so a material on a nameplate reprojects under TAA
like the rest of the widget.
