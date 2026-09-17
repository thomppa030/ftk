# Motion Field

A world-space record of what has moved through the world lately: a top-down map
over a square that follows the camera, splatted each frame by whatever is
moving and fading back to still.

It exists so that more than one system can answer movement without each of them
gathering the movers itself. Fog stirred by a runner reads it today; grass bent
aside, a track through snow and ripples on a puddle are the sort of thing it is
shaped for.

## Being noticed

A node is noticed by carrying a **Motion Source** component. A character gets
one when physics gives it a capsule, so a player is a source without anyone
asking; everything else opts in.

- **Radius** — how wide a mark it leaves. Left at 0 it follows the node's own
  size where there is one to follow, and half a metre where there is not.
- **Height** — how far above the node the movement reaches, so a consumer can
  leave the air over a walker's head alone. 0 follows the node's size too.
- **Strength** — scales the mark. Below 1 for something that barely disturbs
  the air, above it for something that shoulders through.

Nothing about this is character-shaped. A vehicle, a rolling boulder or a spell
effect stirs the world by carrying the component, rather than the gathering
growing a case for each new kind of thing.

Something standing still keeps its own ground marked but stirs nothing new —
which is the difference between the map needing another frame of work and
having settled. A scene where everyone has stood still for a while costs
nothing at all.

## What a texel holds

Velocity on the ground plane, how high the movement reached, and how recently
it happened.

**What recency is worth is the reader's to decide.** The map records *when* and
nothing else, because grass springs back in a moment and fog settles over
seconds. It remembers for four seconds — long enough for the slowest reader —
and a faster one fades its own reading sooner. A reader wanting "the air here
is still being pushed" rather than "something passed here at some point" wants
the top of the range and not the whole of it — but be careful what that drives.
Recency is re-stamped whenever a source passes and decays in between, so it is
a noisy thing to steer anything the eye reads as steady: driving how strongly
fog filters itself over time by it makes the filtering wobble, which shows as
flicker. Let it pick *what* a reader does, rather than how hard something
smooths.

A mark is not a line. It widens as it ages, and rolls as it widens, so a wake
opens out behind whatever left it and breaks into eddies rather than softening
into a band.

## Reading it

From a shader, include `fjell_motion_field.glsl`, with the environment UBO and
a `sampler2D` on the `motion_field` global binding declared before it:

```glsl
MotionSample m = motion_at(world_pos);
// m.velocity  m/s on the ground plane
// m.height    world Y of the top of what came through
// m.recency   1 at the moment of passing, fading to 0

MotionGradient g = motion_gradient(world_pos, MOTION_FIELD_TEXEL_M);
// g.direction  unit, pointing away from the path taken
// g.sharpness  how fast recency falls off, per metre
```

`motion_at` returns zeroes outside the square and when nothing has moved, so a
reader multiplying by `recency` needs no test of its own. The gradient is what
tells one side of a wake from the other without the map storing where anything
was: its direction is unit length, so how hard a reader shears does not change
as the mark spreads.

## The square

64 m across at 512 texels, which is 12.5 cm a texel, following the camera.

It is snapped to whole texels, so what the map holds stands still on the ground
as the camera moves. A map that re-centred every frame would resample its whole
contents each time and shimmer — and would visibly melt when the camera moved
and sharpen when it stopped.

Past the square's edge nothing is known to have moved, and the edge fades so
the border does not read as a line across the world.

## The fog density field

The motion field's second reader, and the one that shows. It is a box of air
around the camera holding how stirred each part of it is — written by
`FogDensityFieldPass`, read by fog, and documented here because the two belong
together: the map says something passed, the field says what the air did about
it afterwards.

A texel holds a signed share of the fog around it: negative where the air was
shoved out of the way, positive where it piled up. Air is displaced rather than
destroyed, so a wake carries its own heaps beside its trough — and because the
two very nearly cancel, the disturbance settles by spreading into itself rather
than being faded out from under the viewer.

Each frame the field is carried forward: advected along a curl of noise (which
turns a wake rather than blurring it), drifted downwind, leaned upward a little
where the air piled up, spread into its neighbours and faded. Whatever moved
through is then carved in. So a wake keeps drifting and fraying for seconds
after whatever made it has gone.

This is what dragging a fog volume's noise about cannot do. That drag can only
move structure the volume already had, and a wisp is metres across where a wake
is not; here the wake *is* the structure, at the size of a texel. 128 × 32 × 128
over 64 m × 16 m — half a metre a texel, 2 MB for the pair.

Fog reads it as a share of whatever fog is already there, so a wake is as strong
as the bank it is carved in and cannot light up clear air. It reaches the global
height fog as well as placed volumes, and needs no property raised: a scene
nobody has walked through costs nothing.

## For engine work

`MotionFieldPass` (`RenderPhase::Environment`, sub-order 70) owns two images and
ping-pongs them: a frame reads the map it is not writing, which one image could
not do — one invocation's read and another's write would race over the same
texel. Which map a frame writes is decided in `prepare_frame`, because that is
where the global binding can be pointed at it; a descriptor may not be
rewritten once a command buffer referencing it is in flight.

Consumers reach it through the global set rather than the framegraph catalog,
gated on `env_ubo.motion_field_texture`.

Sources are gathered from the scene into `EnvironmentState::motion_sources`, so
the renderer never reaches into physics. The cap is 64 in a frame.

`FogDensityFieldPass` (sub-order 71) follows the same shape one dimension up,
with two rules of its own. It is carried forward once a frame however many
viewports draw it — it is one thing in the world, not one per view, and
stepping it twice would advance it over the same span of time twice. And its
box only moves on a frame that dispatches: moving it without a dispatch to
resample into it would leave the contents describing one patch of world while
everything reading them assumed another.
