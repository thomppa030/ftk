# Wakes

What moving things leave on the water: the rings ahead of a wader, the V
behind a boat, the foam on either's crests. A wake is a term of the wave
field, so the surface it moves is the one the player sees, the one a second
boat rocks on, and the one foam gathers on.

## Being noticed

A node stirs the water by carrying a **Motion Source** component, the same one
the [motion field](../Environment/Motion-Field.md) reads. A character gets one
when physics gives it a capsule; a boat, a barrel rolling off a pier or a
spell opts in by adding the component. Radius, height and strength on it mean
what they mean there: how wide the mark is, how far up the body reaches, how
hard it disturbs what it moves through.

A source stirs a body of water only while it is in it: over it, with the
surface between its feet and its head. A walk along a cliff above the sea
leaves the sea alone, and so does a boat carried over it on a trailer.

Nothing is authored on the water. A `.fjwater` says what the sea is; a wake is
what happened to it, derived every frame and never saved.

## What a wake is made of

Every source lays a **trail** of stamps as it moves, one every half
wavelength along its path, and every stamp is the centre of a **ring**
spreading outward at the deep-water speed of its wavelength. The wavelength is
four radii of the source: a person's ripples are about a metre and a half
across, a boat's ten metres, and neither is a dial.

Rings from stamps laid along a path superpose. Behind anything faster than its
rings spread they line up into a V; ahead of anything slower they run on ahead.
A wader pushes ripples before it and a sprinter drags a wake, from one formula,
and the angle of the V follows from the speeds rather than from a setting.

A ring is one crest and flat water either side: a raised cosine across a
quarter-wavelength band. A real ring has troughs too, but two dozen of those
along a path add and cancel into a wave tank, where thin crests stay
separate arcs whose envelope is the V. Each ring decays over a few seconds and
spreads out with its circumference, so a trail fades from the far end. A
stamp is forgotten at six seconds.

The look — amplitude per metre of radius, crest width, decay — lives in one
place, `WakeLook` in `water_wake.hpp`. If a pond and a sea ever
want different answers it becomes a `.fjwater` block; until then it is the
engine's.

## Vertical only

A wake lifts and lowers the surface and never moves it sideways. That is a
deliberate departure from the Gerstner trains, and it buys three things: a
wake can never fold the surface through itself, it draws nothing from the
steepness headroom the trains are budgeted against, and it adds nothing to
the Jacobian the crest mask is taken from. Foam on a wake is its own crests,
and comes from `WAKE` below.

## What moves the mesh and what only shades it

The surface is drawn from a grid — a metre between vertices on the clipmap's
finest level — and a ring narrower than four vertices cannot be drawn as
displacement without aliasing. So the split is by the source's size against
the water's vertex spacing:

- **Wider than the spacing** (a boat): the wake **moves the mesh**. Height and
  slope go into the mesh shader's sums beside the trains and the simulated
  sea, and the CPU sums the same, so buoyancy feels it. On a clipmap level
  too coarse for its crests the term fades out — full at four vertices across
  a crest, gone at two — and on an edge shared with a coarser level it
  fades by the coarser level's spacing, so the two levels agree. The CPU does
  not mirror the fade: a body far enough away to be on a faded level cannot be
  seen floating inside a ripple.
- **Narrower than the spacing** (a player): the wake **shades the surface**.
  Its slope tilts the normal per pixel in the fragment stage, from the same
  stamps, faded by how many metres a pixel covers so a ripple narrower than a
  few pixels never shimmers. The CPU ignores these entirely: nothing floats on
  a swimmer's ripple.

Both kinds foam.

## Foam: the `WAKE` built-in

`fragment()` in a `shader_type water` material sees **`WAKE`**, 0..1: how much
foam the wake has put here. It is the strongest ring crest under the pixel, as
the share of its birth height the crest still has, with a faster fade of its
own on top so the foam is gone before the ripple is. Foam therefore sits on
the V arms and the bow arcs by construction,
and dies with the rings. It is the strongest crest rather than a sum, so
crossing crests do not whiten where they cross.

There is deliberately no foam along the path itself, and no bow heap. A tube
of churn behind the body was tried and read as a painted line; a dipole heap
ahead of it read as a lump, and the freshest ring, leaving the body's edge as
it is laid, already is the bow wave. The crests are where the eye expects foam
on a wake.

What foam that is stays the material's business. `water_default.fjsl` foams it
the way it foams `CREST`; a shader with a foam texture, a paint style or a
lace cuts the wake's crests the way it cuts its whitecaps.

## Buoyancy

**A body does not feel its own wake; other bodies do.** Feeding a body its own
output back is a loop, and a second boat crossing the first one's wake should
rock. `surface_motion()` takes the node to leave out, and buoyancy passes the
body's own.

Trails are laid from replicated positions, so two machines in a networked
game agree closely on a wake but not bit for bit. The difference is bounded
by the wake's amplitude and decays within seconds; the sea under it is still
identical everywhere.

## Evaluated twice

Like the trains, the wake is a pair: `shaders/include/fjell_water_wake.glsl`
for drawing, `src/renderer/resources/water_wake.cpp` for physics, in the same
shape. `tests/test_water_wake.cpp` compiles the shader half as C++ through
`tests/glsl_shim.hpp` and compares the two at a grid of points and times. It
also checks the slope against a finite difference of the height and the
velocity against a finite difference in time, so the GPU half is held to
arithmetic and not only to its twin, and it checks that a scene with nothing
moving leaves the surface bit for bit as it was.

The records reach the GPU as one std430 buffer per frame, every water node's
wakes in it and each draw handed its range through the push block. The shader
reads a record **in place, by index**. A record is 480 bytes; copied into a
local, four times a vertex, it took the mesh shader past what it is given to
work in and lost the device. That is the rule `fjell_water_field.glsl` states
for the field, and it holds here.

## Limits

- Sixteen sources per body of water, twenty-four stamps each. Past that the
  newest sources are not followed.
- Rings do not disperse: every ring of a source spreads at one speed. The
  chirp of a real wake is invisible at gameplay distance.
- A wake does not reflect off a hull or run up a shore, and does not reach the
  fog or the SDF.
- Swimming is not this: the character has no water state. This is the mark a
  body leaves, not how it moves.
