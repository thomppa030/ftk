# Post-Processing

How the finished frame looks in Fjell is an FJSL shader you write. The tone
curve, a colour grade, a vignette, a stylised filter: all of it is a
`shader_type post;` material on a **Post Process Volume**, the way a fog bank's
look is a `shader_type fog;` material on a Fog Volume.

```glsl
shader_type post;

uniform int operator : hint_enum("Linear,Reinhard,ACES,AgX,Hable,Neutral") = 3;
uniform float bloom_intensity : hint_range(0.0, 1.0, 0.01) = 0.3;
uniform sampler2D grade : hint_lut;
uniform float saturation : hint_range(0.0, 2.0, 0.01) = 1.0;
uniform float vignette : hint_range(0.0, 1.0, 0.01) = 0.25;

void color() {
    vec3 c = lut_sample(grade, tonemap(operator, COLOR + BLOOM * bloom_intensity));
    float grey = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = mix(vec3(grey), c, saturation);
    vec2 d = SCREEN_UV - 0.5;
    COLOR = c * (1.0 - vignette * dot(d, d) * 2.0);
}
```

Put it in a `.fjmat` and set that on a Post Process Volume's Material field.
With no volume in the scene, or a volume with no material, the frame draws
with the engine's own `shaders/post.fjsl`: the bloom, the AgX curve and an
unset grade. That file is an ordinary post shader and the place to start
from.

## What a post shader is

A post shader is a complete function from the scene's colour to the colour on
screen. One hook, `color()`, which is named for what it writes:

- `COLOR` comes in as the scene after TAA, times the exposure: linear, HDR,
  anything from zero up.
- `COLOR` goes out display-linear, 0 to 1 expected.

The tone curve is the shader's, so a shader that forgets it shows the exposed
scene clamped (the parser warns about a post shader with no `color()` at all).
After the hook, the engine keeps the result non-negative, encodes it to sRGB
and dithers it for the 8-bit output; an author never sees the output format.

`fragment()`, `vertex()`, `light()`, `sky()`, `layers()` and `density()` are
refused in a post shader, and `color()` is refused in every other type.

### Built-ins

| Built-in | Type | What it is |
|---|---|---|
| `COLOR` | `vec3` | In: the scene after TAA times `exp2(EXPOSURE)`. Out: the display colour, linear. |
| `BLOOM` | `vec3` | The bloom chain's full-resolution mip, exposed the same way. Zero when the viewport runs no bloom. |
| `HAS_BLOOM` | `bool` | Whether `BLOOM` holds anything. |
| `EXPOSURE` | `float` | The EV applied to `COLOR` and `BLOOM`: the project's exposure, adapted when auto exposure is on, plus the day profile's bias. |
| `LUMINANCE` | `float` | The exposure histogram's average scene luminance; 0 when auto exposure is off. |
| `SCREEN_UV`, `PIXEL`, `VIEWPORT_SIZE` | `vec2` | Where on the screen, 0..1 and in pixels, and how big the screen is. |
| `DEPTH` | `float` | Linear depth in metres at this pixel. |
| `DEPTH_TEXTURE` | `sampler2D` | The depth itself: the opaque scene with the water laid over it, so a sea pixel is the sea and not its bed. |
| `TIME`, `TIME_OF_DAY`, `SUN_DIR` | | As the sky has them. |
| `WIND_DIR`, `WIND_SPEED`, `CLOUD_COVERAGE`, `PRECIPITATION` | | The weather, as the sky has it. `WIND_SPEED` carries the gust. |

Helpers in scope: `tonemap(op, c)` with `TONEMAP_LINEAR`, `TONEMAP_REINHARD`,
`TONEMAP_ACES`, `TONEMAP_AGX`, `TONEMAP_HABLE` and `TONEMAP_NEUTRAL`, the six
curves by name (`tonemap_agx(c)` and the rest), `linear_to_srgb` /
`srgb_to_linear`, and `lut_sample(table, c)` for a colour grade (below).

A post shader has uniforms like any other: `float`, `vec2..4`, `int`,
`sampler2D`, with the same hints. `hint_enum` is how the default offers its
curve as a drop-down.

## Post Process Volumes

The hierarchy's **+** menu has **Post process volume**. It is a box or a sphere from the
node's transform, like a fog volume, and the look applies while the camera is
inside it.

| Field | What it does |
|---|---|
| Material | The `shader_type post` `.fjmat`. Empty draws the engine's default. |
| This node | Values for this volume alone, over the `.fjmat`. Beside a material it lists only these, and **Override for this node** takes another; the material's own inspector above edits the shared asset. Changing the material clears them, since they were set against the old look. |
| Infinite bounds | The volume is everywhere and its shape is ignored: the scene's base look. |
| Shape, Edge falloff | Where it applies, and the fraction of its size the look fades in across, so walking in blends rather than cuts. |
| Priority | Where volumes overlap, higher applies over lower. |
| Weight | How much applies with the camera fully inside, 0..1. A script fading a look in or out drives this. |
| Group | For `post.<group>.<uniform>` tracks. |

A scene's usual setup is one volume with infinite bounds for the base look,
and bounded ones where the look should change: a cave, an interior, a
dream sequence's room.

### How volumes blend

Each viewport works this out for its own camera, so a camera preview or a
security camera in another room gets that room's look.

1. Every volume the camera is in counts for its weight times how far inside
   it stands (0 at the outer edge of the falloff, 1 past its inner edge). An
   infinite volume counts for its weight wherever the camera is.
2. They apply lowest priority first, over the engine's default.
3. A volume with the **same shader and textures** as the layer below blends
   its values into that layer. This is the common case, one project post
   shader with different values per volume, and it stays one draw however
   many volumes overlap.
4. A volume with a **different shader or texture** draws its own layer over
   the ones below, blended by how much it counts. Values cannot blend across
   shaders, so the finished images do. At full weight it covers what is under
   it, and what is under it is not drawn.

A second draw costs only while the camera stands in a blend between two
different shaders. Every layer reads the scene's HDR colour, so a volume can
change the tone curve, not just the grade.

## Tracks

A [day profile](Environment/Day-Profiles.md) or a
[weather event](Environment/Weather.md) drives post values the way it drives
fog:

- `post.<uniform>` reaches every volume with infinite bounds, the base look.
- `post.<group>.<uniform>` reaches every volume in that group, bounded or not.

The track editors offer every uniform the scene's post materials declare, with
the range the shader gave it (an enum's runs over its names), and say so
beside a track that reaches nothing.

## Colour grading with a LUT

A grade made in an image editor or in Resolve is a table: every display
colour and the colour it becomes. A post shader takes one as a `hint_lut`
sampler and reads it with `lut_sample()`:

```glsl
uniform sampler2D grade : hint_lut;
uniform float grade_amount : hint_range(0.0, 1.0, 0.01) = 1.0;

void color() {
    vec3 c = tonemap(TONEMAP_AGX, COLOR + BLOOM * 0.3);
    COLOR = mix(c, lut_sample(grade, c), grade_amount);
}
```

`lut_sample()` takes the display colour after the tone curve, 0 to 1, and
gives the graded one back. An unset slot reads the neutral table, which
changes nothing, so a shader can carry a grade slot before it has a grade.
The engine's `post.fjsl` does exactly this, with `grade_amount` to fade it.

The slot takes a `.fjlut`, a small recipe naming the table's source:

```json
{ "name": "Dusk", "source": "dusk.png" }
```

The source sits beside the recipe and is either:

- **a strip PNG**, Unreal's layout: N slices of N by N side by side, so
  256×16 or 1024×32. Red runs across a slice, green down it, blue from slice
  to slice. 8 or 16 bits a channel.
- **a `.cube`** from Resolve or Photoshop's export: `LUT_3D_SIZE`, optionally
  `DOMAIN_MIN`/`DOMAIN_MAX`, and the rows with red changing fastest. It is
  resampled onto a 32 strip.

Either way the table is loaded at 16 bits, clamped and filtered, and while the
editor runs, saving the recipe or its source reloads it in place.

### Making a grade

1. Take an **ungraded** capture: the capture dialog has an Ungraded tick, and
   Project Settings → Exposure has the same as a runtime switch. Both draw
   the engine's default look whatever volumes the camera is in.
2. Paste the neutral strip beside it. `assets/grading/neutral_16.png` and
   `neutral_32.png` ship with the engine, or write one with
   `fjimport --neutral-lut out.png --size 32`.
3. Grade the whole image in the editor you like.
4. Crop the strip back out, save it beside a `.fjlut` naming it, and set the
   `.fjlut` on the material's grade slot.

A 16 strip is small and exact for gentle grades; a 32 one holds a strong or
hue-bending grade without banding. Two volumes with different tables blend
their finished images, since tables cannot mix, so a camera walking between
two graded rooms cross-fades rather than steps.

## Exposure

Exposure stays in **Project Settings → Exposure**: auto exposure, the fixed EV,
the adaptation speed. It is a measurement and a correction rather than a
look, so it is not the shader's. The shader sees the result as `EXPOSURE`.

The tone curve used to be a project setting too. A project that saved one other
than AgX logs once on open, naming the curve it used: add a Post Process Volume
with infinite bounds and set its operator to that curve to keep the look.

## For engine work

- Template: `shaders/include/fjell_post_template.glsl`. Set 0 is the global
  set (the UBOs), set 1 the bindless material set, set 2 the pass inputs:
  scene colour, bloom, `surface_depth` and the layer values.
- A post shader's values are per viewport, and one frame renders several
  viewports while the material params buffer is one buffer. So the pass never
  writes a material row's params: it writes each layer's values into a buffer
  of its own per frame in flight and pushes where they start, and the
  generated GLSL reads them there (`post_params[push.param_base + slot]`). The
  material row is only there for textures.
- Each post shader gets two pipelines: an opaque one for the first layer,
  which covers the image, and a blended one for layers drawn over it. The
  blended one blends alpha as "over", which keeps the image opaque; ImGui
  draws the viewport with alpha blending.
- The planner is `plan_post_layers()` in `src/renderer/post_layers.cpp`, plain
  data in and out, tested in `tests/test_post_layers.cpp`. How far inside a
  volume the camera stands is `volume_occupancy()` in
  `src/scene/volume_occupancy.hpp`, the CPU half of the GLSL in
  `shaders/include/fjell_volume_shape.glsl`; `tests/test_volume_occupancy.cpp`
  holds the two together.
- Volumes reach the pass through `EnvironmentState::post_volumes`, resolved
  from the nodes each tick with their material's values, the node's own and
  the tracks folded in. Which apply is left to the pass, because it depends on
  the camera.
- The pass is still the class `TonemapPass`, named `tonemap` in the graphs.
