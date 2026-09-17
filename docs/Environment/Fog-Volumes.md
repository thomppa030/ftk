# Fog Volumes

The scene's [volumetric fog](Weather.md) is one haze over the whole world: a
density that thins with height and is the same everywhere at a given altitude.
A **Fog Volume** is a box or a sphere you place that adds more fog inside it,
so a valley can hold mist that the ridge above it does not, and a cellar can be
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
| **Shape** | Box or Sphere. Under a non-uniform scale a sphere is an ellipsoid. |
| **Density** | How much fog it adds, on top of whatever the global fog already puts there. |
| **Color** | The fog's colour where this volume is thick. |
| **Scattering** | Multiplies how brightly this volume's fog scatters light, without changing how thick it is. Brighter or dimmer, not more or less. |
| **Edge Falloff** | How much of the volume's extent the edge fades across, as a fraction. 0 is a hard edge and looks like one. 0.2 is the default. |
| **Group** | Ties the volume to `fog_density.<group>` tracks in a day profile or a weather event, so the hour can drive it. Empty means nothing but the scene does. See below. |

### Volumes add

Every volume contributes density on top of the global fog and on top of every
other volume, so two overlapping volumes are thicker where they cross and the
order they appear in the scene cannot matter.

Colour and scattering are **averaged by how much density each contributor
puts here**, which is what makes a warm bank read warm in its middle and fade
to the scene's fog colour as it thins. The blend follows the fog you can see
rather than the bounds you cannot.

The deliberate cost: a volume cannot *remove* fog. There is no way to carve a
clear pocket inside a thick haze — that needs a priority or override mode which
does not exist yet.

## Making it move

By itself a volume is a fixed bank. The **Noise** section breaks it into wisps
and drifts them through the world, which is what turns a bank of fog into
creeping morning mist.

Noise is off until **Contrast** is above zero, so a volume you have already
placed does not start moving on its own.

| Field | What it does |
|---|---|
| **Contrast** | How broken up the fog looks, 0 to 1. Deliberately **not** how much fog there is: the field averages back to one, so you can dial wisps in and out without re-tuning Density. |
| **Scale** | Metres per noise period — the size of a wisp. 8 m default. Small and fast is the hardest case for the renderer to keep stable. |
| **Direction** | The way the field drifts. Normalised, so its length does not matter; Speed alone sets the pace. |
| **Speed** | How fast it drifts, in metres per second. |
| **Wind** | How far the weather takes over, 0 to 1. See below. |
| **Wind Scale** | Metres per second of drift per metre per second of wind, at full influence. Only shown when Wind is above zero. |
| **Octaves** | Layers of detail, 1 to 4. Each one doubles what sampling the field costs. |

The field is sampled in **world space**, not the volume's. Two volumes at the
same Scale therefore read as one continuous field rather than two tiled
patches, and moving or scaling a volume does not make the fog inside it swim.

Noise multiplies how far inside the volume a point is, not the density
directly, so the edge still fades and the wisps stay inside the shape instead
of eating through its boundary.

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

## Letting the hour drive it

Fog is a property of weather rather than a weather of its own: a dawn has mist
whether it is clear or overcast. So a volume is driven by **the clock first and
the conditions second**, and both reach it through the same naming convention.

Give the volume a **Group** — `valley`, say. Then in a
[day profile](Day-Profiles.md), add a track called `fog_density.valley`: it
scales the density of every volume in that group, hour by hour. Thick at five,
thinning by eight, gone by nine. `fog_color.valley` does the same for colour.

A `.fjweather` event can carry tracks of the same names, and they apply **on
top** of whatever the hour already made of the volume, weighted by how present
the event is. So a storm at dawn is thicker than a clear dawn, and an event
fading out relaxes its hold rather than dropping it.

```
volume density = what you authored
               × fog_density.valley   from the day profile
               × fog_density.valley   from the playing event
```

Two things worth knowing when you author one:

- A density track wants **Multiply** mode, so it scales what the volume
  authors. A track added by name in the editor starts as Override, which would
  replace the density outright — the mode is a combo on the track, one click.
- A group is just a string. Volumes sharing one move together, which is the
  point: tag every low-lying bank `valley` and one track raises them all.

Volumes with no group are reached by neither, and stay exactly as authored.

## What a volume does not affect

- **Sun shafts.** The shaft march uses the global fog's density alone, so a
  bank will not thicken the god rays passing through it.
- **The camera preview panel.** It renders through `editor_preview.fjgraph`,
  which has no fog pass at all, so volumes are invisible there exactly as sun
  shafts are. Judge fog in the main viewport or in play mode.
- **Occlusion.** Fog receives ambient light wherever it is, so a volume deep
  inside a building is lit by the sky as much as one in the open.
- **The ungrouped `fog_density` track.** In a [day profile](Day-Profiles.md) or
  a `.fjweather` event it means the global haze and nothing else, so a profile
  written before volumes existed still does exactly what it did. Reaching a
  volume takes a group, below.

## For engine work

`FogVolumeNode` derives `VolumeNode`, which carries the shape and the edge
falloff for any placed region — the fog volume is its first user, not its only
intended one. How far inside a volume a point sits is `volume_occupancy()` in
`shaders/include/fjell_volume.glsl`; a volume that needs answering on the CPU
rather than per froxel wants a mirror of that function, and the
`.fjwater`/`tests/test_fjwater.cpp` pairing is the pattern for keeping two such
halves honest.

Volumes are gathered by `EnvironmentResolver` into `EnvironmentState::fog_volumes`,
uploaded to one SSBO, and summed per froxel in `shaders/fog_inject.comp`.
Everything downstream — shadows, light scattering, the phase function, temporal
reprojection — is inherited, because a volume only changes what density a
froxel sees. The cap is 64 volumes in a scene.
