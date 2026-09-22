# Fog Volumes

The scene's [volumetric fog](Weather.md) is one haze over the whole world. A
**Fog Volume** is a box or a sphere you place that adds more fog inside it, so
a valley can hold mist that the ridge above it does not, and a cellar can be
thick while the street outside is clear.

Add one from the hierarchy's **+** menu, under **Fog Volume**. It needs a
Volumetric Fog node in the scene to add to.

## Placing one

The node's **transform is the volume** — position, rotation and scale together.
There is no separate size field to keep in step with it: the shape is the unit
box or the unit sphere in the node's own space, so scaling the node to
20 × 3 × 40 gives you a bank forty metres deep and three high, and rotating the
node rotates the bank.

Selecting the volume draws its bounds in the viewport. Fog has no hard edge, so
that wireframe is the only way to see where it actually reaches.

| Field | What it does |
|---|---|
| **Material** | The `.fjmat` the bank draws with, a [fog shader](Fog-Shaders.md). Empty draws the engine's `fog_volume.fjsl`: one colour, broken into wisps by a drifting noise field. Its values are edited right under the field and save with the material. |
| **This Node** | The node's own values for the material's uniforms, for the one bank thicker than the thirty that share its material. |
| **Shape** | Box or Sphere. Under a non-uniform scale a sphere is an ellipsoid. |
| **Edge Falloff** | How much of the volume's extent the edge fades across, as a fraction. 0 is a hard edge and looks like one. 0.2 is the default. |
| **Group** | Ties the volume to `fog.<group>.<uniform>` tracks in a day profile or a weather event, so the hour can drive it. Empty means nothing but the scene does. See below. |

The default material's own values:

| Uniform | What it does |
|---|---|
| `density` | How much fog it adds, on top of whatever the haze already puts there. |
| `color` | The fog's colour where this volume is thick. |
| `scattering` | How brightly this volume's fog scatters light, without changing how thick it is. Brighter or dimmer, not more or less. The bank's own value, not a multiplier on the haze's. |
| `noise_contrast` | How broken up the fog looks, 0 to 1. Deliberately **not** how much fog there is: the field averages back to one, so you can dial wisps in and out without re-tuning density. |
| `noise_scale` | Metres per noise period — the size of a wisp. 8 m default. Small and fast is the hardest case for the renderer to keep stable. |
| `noise_octaves` | Layers of detail, 1 to 4. Each one doubles what sampling the field costs. |
| `drag`, `curl`, `thin`, `thin_smooth` | What answering movement is worth; see below. |

### Volumes add

Every volume contributes on top of the haze and on top of every other volume,
so two overlapping volumes are thicker where they cross and the order they
appear in the scene cannot matter. Each carries its own colour and brightness
into the sum, weighted by how much fog it puts there, which is what makes a
warm bank read warm in its middle and fade to the haze's colour as it thins.

The deliberate cost: a volume cannot *remove* fog. There is no way to carve a
clear pocket inside a thick haze — that needs a priority or override mode which
does not exist yet.

## Making it move

By itself a bank is a fixed shape. Two things turn it into creeping morning
mist: the material breaks it into wisps, and the node drifts those wisps
through the world.

The wisps are the material's: raise `noise_contrast` above zero and set the
wisp size with `noise_scale`. The default material samples its noise in
**world space**, so two banks at the same scale read as one continuous field
rather than two tiled patches, and moving or scaling a bank does not make the
fog inside it swim.

The drift is the node's, under **Drift**, because it is accumulated frame by
frame rather than computed from the clock: a heading derived from the time
would jump the moment the wind turned it, and a field that jumps tears.

| Field | What it does |
|---|---|
| **Direction** | The way the field drifts. Normalised, so its length does not matter; Speed alone sets the pace. |
| **Speed** | How fast it drifts, in metres per second. |
| **Wind** | How far the weather takes over, 0 to 1. See below. |
| **Wind Scale** | Metres per second of drift per metre per second of wind, at full influence. Only shown when Wind is above zero. |

The shader reads where the field has got to as `DRIFT` and offsets its noise
by it. A material of your own can do anything with that, or ignore it.

### Wind

**Wind** hands the drift over to the weather. It is not an extra push on top of
what you authored — it steers:

- At **0** the volume drifts exactly along Direction at Speed. This is what a
  cave or an interior wants.
- At **1** it runs downwind at the wind's own pace, and a calm day nearly
  stills it.
- In between, both the heading and the pace are mixed toward the wind's.

**Wind Scale** is there because fog does not travel as fast as the air carrying
it; 0.15 is the default, so a 10 m/s wind drifts the field at 1.5 m/s.

Two deliberate omissions. It reads the **sustained** wind and not the gust — a
gust breathes about once a second, and fog has more mass than that. And the
drift is accumulated frame by frame rather than computed from the clock, so
when the weather turns the field curves round instead of jumping.

## Answering what moves through it

**Response**, under Motion, is how far a volume answers things moving through
it. At 0 it ignores them, which is what a distant bank or a volume nobody walks
into wants. Raise it and something crossing the volume drags the wisps along
behind it, shears the two sides of its wake apart, and leaves the air briefly
thinner where it passed — all of it fading as the [motion
field](Motion-Field.md) forgets.

How much of each of those there is belongs to the material: the default's
`drag`, `curl`, `thin` and `thin_smooth` are the dials, and a material of your
own reads `MOTION`, `MOTION_FALLOFF` and `MOTION_RESPONSE` and answers however
it likes. **Vortices** and **Compression** stay on the node: they drive the
density field simulation the engine runs, not the look.

Three things worth knowing:

- **It needs wisps to drag.** The drag and the shear move the volume's own
  wisps, so a bank with `noise_contrast` at 0 has nothing to displace and
  answers by thinning alone. That still reads, but the churn is what sells it
  — give a volume meant to be walked through some noise.
- **It reaches no higher than what came through.** The motion field records how
  high a mark reached, so a walker stirs the fog around their legs and leaves
  what is over their head alone.
- **It only answers things that say they move.** A node needs a Motion Source
  component to be noticed; a character gets one with its capsule.

The same Response exists on the **Volumetric Fog** node, for the haze, but
expect much less from it. How bright a haze looks along a ray is set by the
whole column, and a walker disturbs a few metres of it: what is lost to
thinner air is very nearly repaid by light arriving from the fog behind, so in
an open scene the two cancel almost exactly however high Response is set. A
volume is a large enough share of the fog along the ray for the same thinning
to show plainly. **If you want a wake that carries, put a fog volume where it
should happen.**

## Letting the hour drive it

Fog is a property of weather rather than a weather of its own: a dawn has mist
whether it is clear or overcast. So a volume is driven by **the clock first and
the conditions second**, and both reach it through the same naming convention.

Give the volume a **Group** — `valley`, say. Then in a
[day profile](Day-Profiles.md), add a track called `fog.valley.density`: it
scales the density of every volume in that group, hour by hour. Thick at five,
thinning by eight, gone by nine. `fog.valley.color` does the same for colour,
and `fog.valley.noise_contrast` can break the banks into wisps as the sun comes
up. Any uniform the group's material declares can be named; the editor offers
the ones the scene has.

A `.fjweather` event can carry tracks of the same names, and they apply **on
top** of whatever the hour already made of the volume, weighted by how present
the event is. So a storm at dawn is thicker than a clear dawn, and an event
fading out relaxes its hold rather than dropping it.

```
volume density = the material's, under the node's own value
               × fog.valley.density   from the day profile
               × fog.valley.density   from the playing event
```

Two things worth knowing when you author one:

- A density track wants **Multiply** mode, so it scales what the volume
  authors. The editor offers it that way; a colour or any other uniform is
  offered as Override, which replaces the value.
- A group is just a string. Volumes sharing one move together, which is the
  point: tag every low-lying bank `valley` and one track raises them all.

Volumes with no group are reached by neither, and stay exactly as authored.
The older names `fog_density.<group>` and `fog_color.<group>` still work.

## What a volume does not affect

- **Sun shafts.** The shaft march uses the haze's density alone, so a bank
  will not thicken the god rays passing through it.
- **The camera preview panel.** It renders through `editor_preview.fjgraph`,
  which has no fog pass at all, so volumes are invisible there exactly as sun
  shafts are. Judge fog in the main viewport or in play mode.
- **Occlusion.** Fog receives ambient light wherever it is, so a volume deep
  inside a building is lit by the sky as much as one in the open.
- **The ungrouped `fog.density` track.** In a [day profile](Day-Profiles.md) or
  a `.fjweather` event it means the haze and nothing else. Reaching a volume
  takes a group, above.
- **The haze's `light()`.** A bank's material may define one, but only the
  scene's fog node lights the fog; the bank's is reported and ignored.

## For engine work

`FogVolumeNode` derives `VolumeNode`, which carries the shape and the edge
falloff for any placed region — the fog volume is its first user, not its only
intended one. How far inside a volume a point sits is `volume_occupancy()` in
`shaders/include/fjell_volume.glsl`; a volume that needs answering on the CPU
rather than per froxel wants a mirror of that function, and the
`.fjwater`/`tests/test_fjwater.cpp` pairing is the pattern for keeping two such
halves honest.

Volumes are gathered by `EnvironmentResolver` into `EnvironmentState::fog_volumes`
with their material's uniforms resolved, uploaded to one SSBO, and drawn one
dispatch each into the accumulated medium by `FogMediumPass`, over only the
froxels their bounds project to. Everything downstream — shadows, light
scattering, the phase function, temporal reprojection — is inherited, because
a volume only changes what medium a froxel holds. The cap is 64 volumes in a
scene. How the fog type and its two variants are built is in
[Fog Shaders](Fog-Shaders.md).
