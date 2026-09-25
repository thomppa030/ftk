# Fog Shaders

How fog looks in Fjell is an FJSL shader you write. The scene's haze and every
placed fog bank draw with a `shader_type fog;` material: what the medium is at
a point, and how it scatters each light toward the eye.

```glsl
shader_type fog;

uniform float density : hint_range(0.0, 0.2, 0.0005) = 0.003;
uniform float height_falloff : hint_range(0.01, 5.0, 0.01) = 0.5;
uniform vec3 color : hint_color = vec3(0.8, 0.85, 0.9);
uniform float anisotropy : hint_range(-0.99, 0.99, 0.01) = 0.7;

void density() {
    float h = WORLD_POS.y - ORIGIN.y;
    DENSITY = density * exp(-height_falloff * max(h, 0.0));
    COLOR = color;
}

void light() {
    float phase = LIGHT_KIND == FOG_LIGHT_AMBIENT
        ? ISOTROPIC_PHASE
        : min(henyey_greenstein(anisotropy, COS_THETA), 0.1);
    SCATTERED += LIGHT_COLOR * phase * SHADOW;
}
```

Put it in a `.fjmat` and set that on the **Volumetric Fog** node's Material
field, or on a **Fog Volume**'s. A node with no material draws with the
engine's own: `shaders/fog.fjsl` for the haze, `shaders/fog_volume.fjsl` for a
bank. Both are ordinary fog shaders and the place to start from.

## What a fog shader is

A fog shader answers *what the medium is at a world point*. It is named for
what it writes, not for how the engine samples it: today that is a froxel grid
in front of the camera, and a fog shader knows nothing about froxels.

Two hooks:

- **`density()`** runs per point and decides the medium there: `DENSITY`,
  `COLOR` and `SCATTERING`. Every source of fog runs its own, the haze over
  the whole grid and each bank over the froxels its bounds reach, and the
  results add.
- **`light()`** runs per light per point over the sum, and decides how the
  medium scatters that light: the phase lobe, what the shadow does, whether
  ambient counts. Only the haze's material lights the fog. A bank's material
  may define `light()`, but it is reported and ignored: a bank adds medium,
  the scene's fog shader lights it.

Leaving `light()` out gives the engine's default, the forward-scattering lobe
the fog always used, clamped so the sun brightens the fog toward it without
smearing a blob across it.

A shader may `return;` early from either hook.

## Sources

A medium dispatch is one source of fog: the Volumetric Fog node's haze, which
reaches everywhere and writes first; each Fog Volume, over the froxels its
shape reaches; and each body of water that names a **Medium** material, over
every froxel that lies inside the water along its ray. Which froxels those are
is told from the surface depth the water pass leaves against the scene depth:
between the drawn surface and the scene behind it when the camera is above,
from the eye to whichever ends the water when it is under, so a water's
`density()` never has to say where the surface is: `OCCUPANCY` is one
and `ORIGIN` is the water node, whose y is the surface at rest. Sources add.
`shaders/water_medium.fjsl` is the engine's water medium. See
[Underwater](../Water/Underwater.md).

## `density()` built-ins

`DENSITY`, `COLOR` and `SCATTERING` are the outputs. Everything else is an
input.

### Outputs

| Built-in | Type | What it is |
|---|---|---|
| `DENSITY` | `float` | How much medium is here. Starts at 0. This is the extinction, in the unit the fog node's density field always had. |
| `COLOR` | `vec3` | The medium's colour, linear. Starts white. |
| `SCATTERING` | `float` | How brightly it scatters light, without being thicker. Starts at 1. Each source's own value; a bank's is not a multiplier on the haze's. |

### Where

| Built-in | Type | What it is |
|---|---|---|
| `WORLD_POS` | `vec3` | The point being asked about. |
| `ORIGIN` | `vec3` | The owning node's world position: the height the default haze falls off from, a bank's centre. |
| `LOCAL_POS` | `vec3` | The point in the node's own space. For a bank, the unit box or sphere, −1 to 1; for the haze, `WORLD_POS − ORIGIN`. |
| `OCCUPANCY` | `float` | How far inside the bank's shape the point sits, 0 at the edge and 1 well within, with the node's edge falloff applied. 1 everywhere for the haze. **Multiply your density by it**, or the bank has a hard edge. |
| `VIEW_DIR` | `vec3` | The direction the eye looks along through this point. |
| `VIEW_DIST` | `float` | Metres from the eye. |
| `CAMERA_POS` | `vec3` | The eye's world position. |

### Motion

| Built-in | Type | What it is |
|---|---|---|
| `DRIFT` | `vec3` | How far the bank's node has carried its field so far, in world metres. Zero for the haze. Offset your noise by it: `WORLD_POS + DRIFT` drifts with the wind the way the node was told to. See [Fog Volumes](Fog-Volumes.md). |
| `MOTION_RESPONSE` | `float` | The node's Response, 0 to 1: how far it wants to answer things moving through it. |
| `MOTION` | struct | The [motion field](Motion-Field.md) at this point: `.velocity` (`vec2`, m/s on the ground plane), `.height` (world Y of the top of what came through), `.recency` (1 as it passes, fading to 0). |
| `MOTION_FALLOFF` | struct | Which way the wake weakens: `.direction` (`vec2`, unit, pointing away from the path) and `.sharpness`. |
| `STIR` | `float` | The fog density field's signed share here: below zero where air was shoved aside, above where it piled up. The engine already scales the whole medium by it; read it to shape yours further. |
| `MOTION_FADE_METRES` | `float` | How far above `MOTION.height` a wake still reaches. |

### The world

| Built-in | Type | What it is |
|---|---|---|
| `SUN_DIR` | `vec3` | Toward the sun. |
| `TIME` | `float` | Engine seconds. |
| `TIME_OF_DAY` | `float` | The hour, 0 to 24. |
| `WIND_DIR` | `vec3` | The way the wind blows. |
| `WIND_SPEED` | `float` | Metres per second, gusts included. |
| `WIND_DRIFT` | `vec2` | How far the wind has carried things, in metres on the ground plane. For the haze, which has no `DRIFT` of its own. |
| `CLOUD_COVERAGE` | `float` | How much sky has cloud, 0 to 1. |
| `PRECIPITATION` | `float` | How hard it falls, 0 to 1. |

## `light()` built-ins

| Built-in | Type | What it is |
|---|---|---|
| `SCATTERED` | `vec3` | The output. Add what this light scatters toward the eye. The engine multiplies the sum by the medium's colour, scattering and density afterwards, so keep the body about the phase and the shadow. |
| `LIGHT_KIND` | `int` | `FOG_LIGHT_SUN`, `FOG_LIGHT_POINT`, `FOG_LIGHT_SPOT` or `FOG_LIGHT_AMBIENT`. Called once for each. |
| `LIGHT_DIR` | `vec3` | Toward the light. Zero for ambient. |
| `LIGHT_COLOR` | `vec3` | Colour × intensity, with a local light's attenuation and volumetric weight folded in. For ambient, the sky's irradiance times the scene's ambient intensity. |
| `COS_THETA` | `float` | `dot(VIEW_DIR, LIGHT_DIR)`, the phase angle. 0 for ambient. |
| `SHADOW` | `float` | The light's shadow here, cloud shadow included; 1 for ambient. |
| `DENSITY`, `COLOR` | | The medium here as accumulated over every source, for a shader that shades by thickness. |
| `WORLD_POS`, `VIEW_DIR` | | As in `density()`. |

Helpers in scope: `henyey_greenstein(g, cos_theta)` and `ISOTROPIC_PHASE`,
the 1/4π that an even distribution of light scatters by.

## Noise

`volume_noise(pos, drift, scale, contrast, octaves)` is what the default bank
uses: fBm in world space, normalised to a mean of one so `contrast` changes
how broken up the fog looks and not how much there is. `fbm_noise`,
`value_noise` and `curl_noise` from `noise.glsl` are in scope for anything
else.

## Materials and values

A fog material is a `.fjmat` like any other, and the values stack the same way
for the haze and for a bank:

```
.fjmat values  ←  the node's own params  ←  day profile track  ←  weather track
```

- The **material** holds the values thirty banks share. Edit them under the
  Material field on the node; they save with the `.fjmat`.
- **This Node**, below it in the inspector, holds the node's own value for a
  uniform, for the one bank thicker than the rest. With a material it lists
  only those, and **Override for this node** takes another; revert returns it
  to the material's. With no material every uniform is listed there, since the
  engine's default shader is not an asset to edit. Changing the node's
  material clears them: they were set against the old look, and kept they
  would hide what the new one says.
- **Tracks** in a [day profile](Day-Profiles.md) or a
  [weather event](Weather.md) name a uniform: `fog.density` or
  `fog.wisp_contrast` for the haze, `fog.valley.density` for every bank
  grouped `valley`. Any uniform the shader declares can be driven this way.

The engine writes the resolved values into a row of its own each frame and
never into the material's, so a track cannot leak into the asset.

## Hot reload

Saving the `.fjsl` or the `.fjmat` reaches the running scene, in play mode too.
A compile error keeps the last good shader drawing and reports the line in
your file, not a line in the generated one.

## Cost

Both hooks run for every froxel the source reaches, some hundreds of
thousands per frame at a full window. Each octave of noise doubles what
sampling it costs, and `MOTION_FALLOFF` costs four extra motion-field reads.
A bank pays only for the froxels its bounds project to, so a small bank with
an expensive shader is cheap and a haze with one is not.

## For engine work

The type's template is `shaders/include/fjell_fog_template.glsl`, compiled
twice by `PipelineCache` from one generated source: the **medium** variant,
one dispatch per source into the transient `fog_medium` (rgb = Σ colour ×
scattering × density, a = Σ density), and the **light** variant with
`FJELL_FOG_LIGHT`, once over the sum into the scatter volume the temporal and
integrate steps read. `FogCore` in `volumetric_fog_pass.cpp` owns the
per-shader pipelines and a private material row per source. The four fog
compute shaders share one push block, `fjell_fog_push.glsl`, mirrored by
`FogCore::PushConstants`.

The pattern this follows is written up in
[Look Belongs in Shaders](../Architecture/Look-Belongs-In-Shaders.md).
