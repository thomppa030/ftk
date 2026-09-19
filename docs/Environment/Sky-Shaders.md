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
    SKY += star_field(VIEW_DIR, STARS, CELESTIAL_AXIS, CELESTIAL_ANGLE, TIME);
    SKY += moon_disc(VIEW_DIR, MOON_DIR, SUN_DIR, MOON_COLOR, 0.5);
    if (!SKY_FOR_LIGHTING) {
        SKY += sun_disc(VIEW_DIR, SUN_DIR, SUN_COLOR, 1.8, 1.2, 1.0);
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
    SKY += sun_disc(VIEW_DIR, SUN_DIR, SUN_COLOR, sun_size, sun_halo_size, sun_halo);
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
vec3 sun_disc(vec3 view_dir, vec3 sun_dir, vec3 sun_color,
              float radius_deg, float halo_deg, float halo);
vec3 moon_disc(vec3 view_dir, vec3 moon_dir, vec3 sun_dir, vec3 moon_color, float radius_deg);
vec3 star_field(vec3 view_dir, float intensity, vec3 axis, float angle, float time);
```

- `radius_deg` is the disc's angular radius **in degrees**, measured from its
  centre. The real sun and moon are `FJELL_SUN_RADIUS_DEG` (0.27) and
  `FJELL_MOON_RADIUS_DEG` (0.26); `gradient_sky` draws them far larger, at
  1.8 and 0.5, because a true half-degree sun is nine pixels at 1080p and
  reads as a speck. Bear in mind what you see is not the radius but the part
  that clips to white, which bloom then spreads.
  Reach for degrees rather than a cosine: within a degree of the sun the
  cosine is flat to four decimal places, so a disc written in cosines is
  unreadable and easy to get an order of magnitude wrong.
- `halo_deg` is how far past the limb the glow reaches, in degrees, and `halo`
  scales its strength. It is independent of the disc, so a small sun can still
  carry a wide glow — and this, not the disc, is usually what reads as "the
  sun is too big": the glow covers many times the disc's area, so it wins the
  eye even at a fraction of its brightness. It is measured **past the limb**,
  so it stays the same width on screen whatever the disc does — `gradient_sky`
  reaches 1.2°, about fourteen pixels of glow around the sun at any size. The
  falloff is sharp at the limb and thin after it; a gentle ramp over the same
  reach paints a bright ring instead of a glow.
- The disc's **brightness is not the light's intensity**. `sun_color` arrives
  scaled by the directional light, but that is how hard the sun lights the
  ground, a separate question from how bright its disc draws; `sun_disc` takes
  only the hue from it and draws at a fixed `FJELL_SUN_DISC_NITS` just over
  the white point. Turning the directional light down to shrink the sun is
  therefore never the fix: it darkens the scene and leaves the disc where it
  was. `FJELL_SUN_DISC_NITS` is the dial for how big the sun *reads*, as much
  as the radius is — every stop above white widens the blob bloom draws
  without making the sun look brighter.
- `moon_disc` shades the moon as a sphere lit from the sun's direction, so the
  phase you see is geometry rather than a painted shape, with a little
  earthshine so the dark limb stays readable. The environment places the moon
  from its `moon_phase`, which is why that one value can set both the shape
  and the brightness without the two disagreeing.
- `star_field` undoes the celestial rotation on the view direction, so the
  stars are fixed to the sphere and wheel through the night. Pass `STARS`,
  `CELESTIAL_AXIS`, `CELESTIAL_ANGLE` and `TIME` straight through.
  `TIME` drives scintillation, and the field leans on four things to avoid
  reading as a texture dragged across the sky:
  - **Twinkle scaled by altitude.** A star's wobble follows how much air the
    line of sight crosses, so horizon stars shimmer hard and the zenith is
    nearly still. This is the one that matters: a rigid field that only
    rotates is what makes a night sky feel wrong.
  - **A skewed magnitude distribution.** Brightness is a uniform roll raised
    to a power, giving a few bright stars and very many faint ones. A uniform
    roll instead reads as evenly scattered confetti.
  - **Colour from the blackbody locus**, correlated with brightness: the
    bright ones spread toward orange-red and blue-white while the faint
    majority stays near white, which is also how the eye sees them.
  - **Clustered placement.** How many cells hold a star varies across the sky
    and rises sharply in a galactic band, and stars jitter past their own cell
    bounds, so the sky has voids and crowds instead of an even lattice. The
    band also carries a faint unresolved glow.

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

**Coverage thickens the sheet; it does not flatten it.** Rising coverage walks
a threshold down through the cloud field, but the threshold is placed against
that field's own range and stops above its floor — slide it past the bottom and
every pixel lands above the ramp, which paints the whole sky one flat white
sheet. That was the old behaviour at rain, and it is the failure to watch for
if you write your own shaping.

A fully covered sky *is* opaque, so `cloud.a` still reaches ~1 nearly
everywhere at coverage 1. What keeps it from looking blank is the shading:
`cloud_layer` reads the cloud field directly rather than taking its thickness
from the alpha, so the colour keeps its structure exactly where the opacity has
none left to give. A storm reads as a churning mass of dark and light rather
than a blanket. Thin scraps stay bright and take the sun's hue, so they warm
through sunrise and sunset; thick masses settle toward a cool slate that no sun
turns golden, which is what makes heavy weather read blue-grey.

The shape comes from **domain-warped** fbm — the field's coordinates pushed
about by a coarser copy of itself — which gives the billowing, sheared forms
plain fbm cannot, since plain fbm is isotropic about one origin and yields
round blobs. It costs three fbm evaluations where one would do, and the cloud
shadow pass pays that five times per ground texel. Both still use it, because
the shadow is meant to be the cloud's own: shading the shadow from a cheaper
field puts shadows where the clouds are not.

The same noise drives the **cloud shadow map**, so the shadow crossing your
terrain belongs to the cloud overhead. That happens automatically: every
consumer of the sun's shadow multiplies by it, whether or not your sky draws
clouds at all. A sky that ignores `cloud_layer()` still gets cloud shadows on
the ground, which will look wrong — so if your weather has coverage, draw the
clouds.

### Drawn clouds

The sheet draws one kind of cloud: crisp shapes with creased, torn edges
standing against open sky. There is no photoreal mode and no dial between the
two — a mode nobody uses is a mode nobody maintains.

The look is in **how the field is built**, not in anything applied afterwards.
Posterising the sheet into flat levels and inking their boundaries was built
first and thrown away: it reads as a contour map, or as a photograph with the
contrast pushed, because a flat value is the symptom and the silhouette is the
cause. What is there instead:

- **Folded octaves.** `abs()` creases the noise at every zero crossing, giving
  the torn edge a drawn cloud has where plain fbm gives a rounded blob.
- **A multiplicative contrast term.** Multiplying the field by the turbulence
  plus its own magnitude empties most of the sky and leaves the cloud that
  survives with a steep edge. An additive blend only ever produces another
  even wash.
- **A narrow cut.** The field spans `FIELD_HI - FIELD_LO`, so a transition
  width of 0.3 would be a third of everything there is and every silhouette
  would come out soft.

A sky sets the dials in its `sky()` body before calling `cloud_layer()`:

```glsl
fjell_cloud_contrast = cloud_contrast;  // how hard the silhouette is cut
fjell_cloud_wisp = cloud_wisp;          // round shapes vs torn, filamented
fjell_cloud_edge = cloud_edge;          // how far sky takes to become cloud
vec4 cloud = cloud_layer(...);
```

They are globals rather than `#define`s deliberately: this include is expanded
into the sky template *above* the point where a material's uniforms exist, so a
compile-time constant could not be driven per-material and the dial would not
be one. `gradient_sky.fjsl` exposes them as uniforms, so the look rides in the
`.fjmat`.

**The field must stay inside `FIELD_LO..FIELD_HI`.** Those constants are what
every cut measures against, so a field that drifts out of the range silently
rewrites what a coverage value means — the sheet inverts and the sky fills in
where it should be clearing.

#### Cloud size and sheet height

`cloud_size` and `cloud_height` on the sky set how big one cloud is and how
high the sheet sits. They only mean anything **as a ratio**: straight up you
are as close to the sheet as you ever get, so a cloud there subtends
`2*atan(size / 2 / height)` — the widest it can ever look. At 1400 m across a
sheet 1500 m up that is fifty degrees, barely one cloud on screen, which reads
as looking at the sky through a microscope. Real cumulus run 500-1000 m across
with their base at 1000-2000 m; the defaults (600 m at 2200 m) sit in that
range and give about four clouds across a sixty degree view.

Everything else derives from `cloud_size`: the noise scale, the detail budget's
octave sizes, and the cloud shadow's blur radius, which is written in metres
and converted rather than authored in sheet units — a blur in units would
silently change width whenever the cloud size did.

#### Why the sheet is a shell, and why it drops detail

Two things about a cloud sheet only show up at the horizon, and both were real
bugs rather than tuning:

**The sheet is a shell, not a plane.** A plane's distance is
`height / sin(elevation)`, which runs to **infinity** at the horizon: it
foreshortens without limit, so cloud detail compresses into a band of stipple
and then a smear. `fjell_cloud_shell_distance()` intersects a shell at Earth
radius instead, which terminates at about 138 km — a finite far edge is exactly
why real clouds converge to a horizon line. `cloud_shadow.comp` traces the same
shell: plane and shell disagree by 63 m with the sun at 8 degrees and nearly
500 m at 4, against a shadow map at roughly 2 m per texel, so tracing a
different surface there would put the shadow where the cloud is not.

**The field drops octaves with distance** (`fjell_cloud_lod`). This is
mipmapping done by hand, because the field is evaluated rather than sampled.
An octave finer than two pixels cannot be resolved: what it adds is not detail
but noise, and noise that changes as the camera moves is flicker. The budget
comes from the pixel's footprint on the sheet, which the ray's *angle* stretches
as much as its length — a grazing ray covers ground far faster than a steep one.
At 1080p and a 60 degree field of view, roughly eight octaves survive overhead
and none below two degrees.

Two traps worth naming, because both looked right and were not:

- **Dimming far cloud does not fix flicker.** A half-transparent pixel that
  flips between cloud and sky still flips. The detail has to go, not its
  contrast.
- **Distance as a share of the way to the horizon is the wrong variable.** The
  sheet only reaches half the horizon's distance at about *one* degree of
  elevation, so a fade keyed to it reads zero across the entire band that
  actually aliases. The final fade is keyed to the LOD itself, so it begins
  exactly where the budget runs out rather than at a number that would have to
  be rediscovered whenever the feature size or the sheet's height changed.

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
