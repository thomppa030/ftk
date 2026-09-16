# Sky Shaders

A sky in Fjell is an FJSL shader you write. It sees the sun, the moon, the time
of day and the weather, and what it paints is also what lights the world.

```glsl
shader_type sky;

uniform vec3 zenith_color : hint_color = vec3(0.35, 0.58, 0.77);
uniform vec3 horizon_color : hint_color = vec3(0.98, 0.83, 0.63);

void sky() {
    float t = clamp(VIEW_DIR.y * 0.5 + 0.5, 0.0, 1.0);
    SKY = mix(horizon_color, zenith_color, t) * SKY_TINT;
    SKY += star_field(VIEW_DIR, STARS, CELESTIAL_AXIS, CELESTIAL_ANGLE);
    SKY += moon_disc(VIEW_DIR, MOON_DIR, SUN_DIR, MOON_COLOR, 0.9994);
    if (!SKY_FOR_LIGHTING) {
        SKY += sun_disc(VIEW_DIR, SUN_DIR, SUN_COLOR, 0.9995, 1.0);
    }
}
```

Set it on the World Environment's **Sky Shader** field. It overrides the
built-in gradient, HDRI and procedural atmosphere modes.

## The sky lights the world

This is the part worth understanding before anything else.

The same source is compiled **twice**: once as the fullscreen fragment shader
that draws the sky behind the scene, and once as a compute shader that renders
it into the 64² lighting cubemap. That cubemap is what image-based lighting,
DDGI miss rays and reflections read. So a sky you author is not a backdrop; it
is the light in your scene.

The cubemap re-renders whenever the environment changes — the hour, the sun,
the moon, a uniform, a weather tint, or a hot reload of the shader itself — and
not otherwise.

`SKY_FOR_LIGHTING` tells the two apart. It is `true` in the lighting pass. Use
it to skip the sun disc, because the scene already has a directional light for
the sun and drawing the disc into the cubemap counts it twice:

```glsl
if (!SKY_FOR_LIGHTING) {
    SKY += sun_disc(VIEW_DIR, SUN_DIR, SUN_COLOR, sun_size, sun_halo);
}
```

Everything else — the gradient, the clouds, the moon, the stars — should be in
both, because all of it genuinely lights the world.

## Built-ins

`sky()` writes one output, `SKY`, in linear RGB. Everything else is an input.

### The view

| Built-in | Type | What it is |
|---|---|---|
| `VIEW_DIR` | `vec3` | The direction this pixel looks in. Normalized. |
| `CAMERA_POS` | `vec3` | The eye's world position. Needed for world-space clouds. |
| `SKY_FOR_LIGHTING` | `bool` | True while rendering the lighting cubemap. |

### The bodies

| Built-in | Type | What it is |
|---|---|---|
| `SUN_DIR` | `vec3` | Toward the sun. |
| `SUN_COLOR` | `vec3` | Its colour times its intensity. Zero once it has set. |
| `SUN_ELEVATION` | `float` | Radians above the horizon, negative below. |
| `MOON_DIR` | `vec3` | Toward the moon. |
| `MOON_COLOR` | `vec3` | Its colour times its intensity. Zero at new moon. |
| `MOON_ELEVATION` | `float` | Radians above the horizon. |
| `MOON_PHASE` | `float` | 0 and 1 new, 0.5 full. |

### Time

| Built-in | Type | What it is |
|---|---|---|
| `TIME` | `float` | Engine seconds. |
| `TIME_OF_DAY` | `float` | The hour, 0 to 24. |
| `STARS` | `float` | How bright the star field should be. Fades in past dusk. |
| `CELESTIAL_AXIS` | `vec3` | The axis the sky turns about. |
| `CELESTIAL_ANGLE` | `float` | How far it has turned since noon, in radians. |

### Weather

| Built-in | Type | What it is |
|---|---|---|
| `CLOUD_COVERAGE` | `float` | How much sky has cloud, 0 to 1. |
| `CLOUD_DENSITY` | `float` | How thick it is. |
| `PRECIPITATION` | `float` | How hard it falls. |
| `WIND_DIR` | `vec3` | The way the wind blows. |
| `WIND_SPEED` | `float` | Metres per second, gusts included. |
| `WIND_DRIFT` | `vec2` | How far the wind has carried things, in metres on the ground plane. **Use this, not `WIND_SPEED * TIME`** — see below. |
| `FRONT_DIR` | `vec2` | Which way the weather front travels, on the XZ plane. |
| `FRONT_EDGE` | `float` | Where its edge stands across the sky: -1 the windward horizon, 1 the leeward. |
| `FRONT_SOFTNESS` | `float` | How soft that edge is. |
| `SKY_TINT` | `vec3` | The weather's tint. Multiply your sky by it. |
| `LIGHTNING` | `float` | The flash, 0 to 1. Add it to your clouds. |

### The scene

| Built-in | Type | What it is |
|---|---|---|
| `AMBIENT_LIGHT` | `vec3` | The scene's ambient. Useful as the light your clouds are lit by from below. |

## Why WIND_DRIFT and not WIND_SPEED × TIME

Because the wind gusts. Multiplying the *current* speed by total elapsed time
rescales the whole distance travelled every time the speed changes, so clouds
slosh back and forth instead of drifting. The environment integrates the wind
into a distance and hands you that; it only ever increases.

This was a real bug in the first version of the cloud layer, and the symptom
was exactly that sloshing.

## The includes

Three helper includes are available to every sky shader, already included by
the template.

### Bodies — `fjell_sky_bodies.glsl`

```glsl
vec3 sun_disc(vec3 view_dir, vec3 sun_dir, vec3 sun_color, float size, float halo);
vec3 moon_disc(vec3 view_dir, vec3 moon_dir, vec3 sun_dir, vec3 moon_color, float size);
vec3 star_field(vec3 view_dir, float intensity, vec3 axis, float angle);
```

- `size` is the **cosine** of the disc's angular radius. 0.9995 is roughly the
  real sun; smaller numbers are bigger discs.
- `moon_disc` shades the moon as a sphere lit from the sun's direction, so its
  phase follows the two bodies rather than a separate slider, with a little
  earthshine so the dark limb stays readable.
- `star_field` undoes the celestial rotation on the view direction, so the
  stars are fixed to the sphere and wheel through the night. Pass `STARS`,
  `CELESTIAL_AXIS` and `CELESTIAL_ANGLE` straight through.

### Clouds — `fjell_clouds_2d.glsl`

```glsl
vec4 cloud_layer(vec3 origin, vec3 dir, float coverage, float density,
                 vec2 wind_drift,
                 vec2 front_dir, float front_edge, float front_softness,
                 vec3 sun_dir, vec3 sun_color, vec3 moon_color, vec3 ambient);
```

Returns the lit cloud colour in `rgb` and how much of the sky it covers in `a`:

```glsl
vec4 cloud = cloud_layer(CAMERA_POS, VIEW_DIR, CLOUD_COVERAGE, CLOUD_DENSITY,
                         WIND_DRIFT, FRONT_DIR, FRONT_EDGE, FRONT_SOFTNESS,
                         SUN_DIR, SUN_COLOR, MOON_COLOR, AMBIENT_LIGHT);
SKY = mix(SKY, cloud.rgb, cloud.a);
SKY += cloud.rgb * cloud.a * LIGHTNING * 3.0;   // lit from inside by a strike
```

The layer is a sheet at a fixed height **in world space**, so the clouds slide
as the camera moves rather than being painted on the sky dome, and the wind
carries the whole sheet. The front shapes the coverage, so the sky closes from
windward during a weather's approach and opens from the same side in its tail.

The same noise drives the **cloud shadow map**, so the shadow crossing your
terrain belongs to the cloud overhead. That happens automatically: every
consumer of the sun's shadow multiplies by it, whether or not your sky draws
clouds at all. A sky that ignores `cloud_layer()` still gets cloud shadows on
the ground, which will look wrong — so if your weather has coverage, draw the
clouds.

Volumetric ray-marched clouds are future work. They are designed to replace
this include without touching a sky that only calls `cloud_layer()`.

### Wind for geometry — `fjell_wind.glsl`

Not for skies; for spatial shaders with a `vertex()` hook.

```glsl
vec3 wind_sway(vec3 vertex, vec3 wind_dir_object, float wind_speed,
               float stiffness, float time);
```

```glsl
void vertex() {
    vec3 wind = transpose(mat3(MODEL)) * WIND_DIR;
    VERTEX += wind_sway(VERTEX, wind, WIND_SPEED, 0.3, TIME);
}
```

The wind is world space; bring it into object space first, which for a rotation
and a uniform scale is the transpose of the model's upper 3×3. `stiffness` 0 is
grass, 1 does not move. The sway grows with height above the object's origin,
so the base stays put.

## What surfaces see

Lit spatial and terrain shaders get weather built-ins too:

| Built-in | What it is |
|---|---|
| `WETNESS` | How wet exposed surfaces are, 0 to 1. Integrated over time. |
| `SNOW` | How snow-covered they are. |
| `PRECIPITATION` | How hard it is falling right now. |
| `CLOUD_COVERAGE` | How much sky is covered. |
| `WIND_DIR`, `WIND_SPEED` | The wind, in `fragment()` and `vertex()` alike. |
| `TIME_OF_DAY` | The hour. |
| `SKY_EXPOSURE` | How open this point is to the sky: 1 under open sky, 0 under a roof. |

By default the engine applies its own response after your `fragment()` runs:
exposed up-facing surfaces darken and go glossy with wetness, ripple while it
rains, and take a snow layer with a noise-broken edge. To answer the weather
yourself, declare `render_mode no_weather` and use the built-ins directly.

## Uniforms

A sky's uniforms are editable. The World Environment lists them under the Sky
Shader field with the same controls the material inspector uses, and an edit
stores an override on the node, serialized with the scene. A **Reset** button
beside a changed uniform returns it to the shader's own default.

Three things can drive a uniform, later winning over earlier:

1. the `.fjsl` default
2. the node's override
3. a [day profile](Day-Profiles.md) or weather custom track of the same name
4. a script's `Weather::set_sky_param("name", ...)`, cleared with
   `clear_sky_param`

So `uniform vec3 zenith_color` can be picked in the inspector, animated over
the day by a `zenith_color` track, and flashed by a script, with no engine
change. Colours are sRGB everywhere a person sees them.

## The examples

Three sky shaders ship with the engine, referenced as `shaders/<name>.fjsl`.
They live in the engine's tree, so a project uses them without copying.

- **`gradient_sky.fjsl`** — a two-colour gradient with a warming horizon near
  the sun, the moon, stars and clouds. The one to copy when starting your own.
- **`painterly_sky.fjsl`** — fbm cloud bands over a day-to-night gradient.
  Exercises `TIME`, the wind and the weather together.
- **`stylised_sky.fjsl`** — hard-banded toon sky driven by `TIME_OF_DAY`, with a
  flat moon and cut-out clouds. Proof that a sky owing nothing to physical
  scattering still lights the world properly through the cubemap.

The latter two are demonstrations of the built-ins rather than art direction.

## Things that will surprise you

- **Forgetting `SKY_FOR_LIGHTING` double-counts the sun.** The scene gets
  noticeably brighter and the ambient takes on the sun's colour.
- **The cubemap is 64².** Fine detail in your sky does not reach the lighting;
  broad colour does. That is the right trade for what it is used for.
- **A sky shader takes over from an HDRI.** While one is set, the HDRI is not
  used for lighting.
- **Weather clouds need `cloud_layer()`.** Otherwise the ground has cloud
  shadows from clouds the sky does not show.
- **Sky colour tracks in a day profile drive the built-in gradient**, not your
  shader. Name your custom tracks after your own uniforms instead.

## See also

- [Time of Day](Time-of-Day.md) — the bodies and the clock the sky reads
- [Day Profiles](Day-Profiles.md) — animating sky uniforms over the day
- [Weather](Weather.md) — clouds, fronts, lightning and surface response
- [Materials/FJSL](../Materials/FJSL.md) — the shader format itself
