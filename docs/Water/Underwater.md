# Under the water

What the camera sees below the surface, and looking down into it from above.
The plan and its remaining phases: `Plans/active/water/water-underwater-2026-09-22.md`.

## The surface from below

A water material's `fragment()` gets **`BELOW`**: 1 when the camera is under
this water and the eye sees the underside of this pixel of its surface. The
camera's side is decided on the CPU each frame by the water pass, from the
wave field over the camera's own column, and only then is a pixel's underside
considered — a folded crest turns its normal over and would otherwise read as
an underside from the beach. On the underside `WORLD_NORMAL` points at the
eye, `WATER_DEPTH` is the water between the eye and the surface, and
`WATER_DEPTH_VERTICAL` is how far the surface stands above the eye.

The look is the material's. `water_default.fjsl` bends the view ray with
`refract()` at the water/air boundary: outside Snell's window, about 48° from
straight up, light cannot leave the water and the surface mirrors the deep
colour; inside it the whole sky is compressed into the cone, sampled from the
screen copy where `SCREEN_UV_TOWARD(dir)` says the bent direction lands on
screen, and from `sky_cubemap` for the rim beyond the frame's edge. The
underside is written as `EMISSION` with no albedo and no metal: the window is
already light, and a metal there would be painted with the sky cubemap's
irradiance, which is not what an underwater surface reflects. The medium
between the eye and the surface tints it.

## The medium

A WaterComponent names a **Medium**: a `shader_type fog` `.fjmat`, exactly
as a fog volume names its material, drawn by the volumetric fog as one more
source beside the haze and the volumes. Its `density()` sees `ORIGIN`, the
water node, whose y is the surface at rest, and writes `DENSITY`, `COLOR` and
`SCATTERING` as any fog shader does. `shaders/water_medium.fjsl` is the
engine's: clear water that blues with distance and darkens with depth. Mind
the unit: the fog reads `DENSITY` as a tenth per metre of extinction, so
water wants values ten times a fog's — 1.5 is clear coastal water with about
twenty metres of visibility, 3 a harbour.

Which froxels are water is not the shader's question. The fog runs after the
water pass and reads two depths: the **surface depth** the water pass leaves,
which is the surface where one was drawn and the scene everywhere else, and
the opaque scene's own. Where the two differ a surface was drawn in front of
the scene, and with the camera above the water lies between them: beyond the
surface, short of the bed. Where they agree nothing was drawn, and beyond an
object there is no water at all — a rule that filled the column behind every
character and hilltop with water leaked out around their edges as a dark rim.
With the camera under the surface a column holds water from the eye to
whatever ends it, the surface or the bed. So the medium works from a boat as
well: looking down into deep water gives volumetric depth colour, shadows and
shafts, inherited from the fog, where the surface shader alone can only tint.

A water with no medium costs nothing. One with a medium costs one full-grid
dispatch of its `density()`, the same as the haze.

Only the haze's `light()` runs, as for volumes: a medium's own `light()` is
ignored.

## Sun shafts

Under the surface the sun shafts march through the water: the medium's
`density` uniform stands in for the haze's along the whole ray, unscaled,
because water is already in the fog's unit and its beams live within metres.
The surface is what the ray stops at, so every step is in water. From above,
looking down into water, the shafts still stop at the surface: they do not
enter the water from the air side.

## The waterline

A camera within centimetres of the surface has the near plane cut through
the mesh, and the cut leaves pixels that show what is behind the surface
with no surface in front of them: a slit of sky from under the water, a slit
of bed from over it. Those columns begin in the other medium, and the fog has
to know.

So while a surface is within a metre of the camera the water pass runs a
small compute dispatch, one thread per froxel column, that unprojects the
column's near-plane point and evaluates the surface over it from the same
field the mesh draws — trains, the simulated sea, the wakes — and marks the
column when that point and the camera are on different sides. The fog medium
reads the mask beside the surface depth: a marked column starts in water when
the camera is in air and in air when the camera is in water, and the rule
above runs from there. It costs a few thousand field evaluations a frame,
and nothing when the camera is away from the surface.

There is no drawn meniscus line; the medium's own edge is the waterline.

## Breath

Whoever has their head under a water surface breathes out while play runs.
The environment hangs an emitter node at the mouth of each such motion
source, the way the weather hangs its rain over the camera, with the
engine's `engine_assets/effects/breath.fjvfx` (a project replaces it by
shipping a file of the same path, and the same goes for the sprite it uses,
`engine_assets/textures/bubble.png`). Every few seconds a breath goes out: a
burst of a few large bubbles and, over the exhale, a stream of small ones,
each rising on its own buoyancy against drag, wobbling on noise, growing a
little as the pressure drops, spinning slowly, and popping into a spray of
droplets at the surface through a sub-emitter. Going under lets a breath out
at once. A head that surfaces stops breathing out and keeps its node for one
bubble's life, so the last breath finishes rising.

The engine drives only the rhythm and the depth: the period and exhale
length are constants on `EnvironmentSubsystem`, and each emitter's bubbles
are given the life they need to reach the surface at that emitter's own rise
speed, read from its buoyancy and drag, so the look stays in the effect.

## Not yet

Caustics on the bed. Swimming and diving are a separate plan.
