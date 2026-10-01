# Shore

A sea knows the ground under it. Over a terrain, its waves grow as the water
shallows, break once the depth can no longer carry them, and run on up the
sand as swash; behind land that stands between the water and the open sea,
they are calmer. Physics floats bodies on the same sea, so a boat in the surf
or in a cove rides the waves that are drawn there.

## Where the ground comes from

Each fixed step, a body of water finds the terrain under it: the first terrain
in the scene whose field reaches under the water. Its depth anywhere is the
water's rest height (the node's Y) minus the terrain's height there, read
bilinearly between samples the way the terrain draws itself.

The heights are the terrain's own: the CPU array physics collides with, shared
with the water rather than copied, and on the GPU the terrain's height texture.
Sculpting reaches the water at once.

Off the terrain there is no ground to speak of: the water there is deep, and
the open sea unless the lee of the terrain's land reaches it (see
[Shelter](#shelter)). A water with no terrain under it is open water
everywhere.

## What depth does to a wave

Two things, both from linear wave theory:

- **Shoaling.** A wave keeps its frequency as it runs into shallow water and
  slows, and the energy it carries piles up: it grows. The growth is Green's
  law in its exact form, `Ks`, a function of `k·d`, the depth in units of the
  wave's own length. Before it grows it dips a little, to 0.91 around
  `k·d = 1`, which is real and not a fit error.
- **Breaking.** A wave breaks when it stands taller, crest to trough, than
  0.78 of the depth under it. So the shallows hold the sea's significant
  amplitude to 0.39 of the depth, which falls to nothing at the waterline.

Deep water begins at `k·d = 7`, about a wavelength and a tenth, where `Ks` is
within 1e-5 of 1. Deeper than that the shore leaves the sea exactly as it was:
the evaluators take the same path they always did, to the bit.

### One rule, per sum

The sea is two sums: the Gerstner trains and the simulated sea. The simulated
sea reaches the GPU as textures holding every wavelength added together, which
nothing can scale one wavelength at a time, so the shore scales each **sum**
by one factor at each point:

- each sum grows by its own dominant wave's `Ks` — the simulated sea's spectral
  peak, the strongest Gerstner train;
- breaking takes the two together, their shoaled heights adding as variances,
  and whatever share it keeps, both keep.

So a Gerstner swell composed over a simulated sea shoals by the swell's length
and does not drag the chop along with it. Two Gerstner contributors share one
dominant wave. A single regular train breaks a little early, since a sum's
height is its significant amplitude and that counts one train as 1.41 times
its amplitude.

## What it looks like

`coastal.fjwater` in the test project: a 40 m dominant wave at 5.1 s and a
significant amplitude of 2.12 m.

| depth | wavelength | amplitude |
|---|---|---|
| 40 m | 40.2 m | 2.12 m |
| 10 m | 37.5 m | 1.98 m |
| 6 m | 32.9 m | 1.94 m |
| 4 m | 28.5 m | 1.56 m, breaking |
| 2 m | 21.3 m | 0.78 m |
| 1 m | 15.5 m | 0.39 m |
| 0 m | — | 0 |

The surf starts where the shoaled height meets 0.39 of the depth, about 5 m
here: 250 m out on a 1:50 beach, 50 m on 1:10.

What it does not do:

- **The waves keep their heading.** They shorten and slow over the shallows
  (see [Over the shallows](#over-the-shallows)) but do not yet turn to face
  the beach.
- **No lines of surf from a wind sea.** A simulated sea is short-crested and
  comes in as a shrinking chop. Lines of surf belong to a long swell: compose
  a Gerstner swell on top.
- **No curl.** A displaced surface cannot overturn forwards; breaking shows as
  height taken away, and as foam where a material draws it.
- **Chop shoals with its sum's dominant wave**, not by its own length. It is
  held down by breaking where that would show.

## Over the shallows

A wave keeps its frequency as it runs into shallow water, so as it slows it
shortens: a 40 m wave is 33 m long in 6 m of water and 16 m in one. The sea's
closed form cannot take that as a wavenumber that changes with depth — the
wavelength drawn would then be `k + s·dk/ds`, with `s` the distance from the
world origin, wrong by more the further out — so the change comes from where
the sea is **read**. Over the shallows each sum of the sea at a point `p` is
its deep-water self at `p + offset`: the deep sea at a point it has not
reached yet, which is its lag. Read that way its crests are closer together
and slower, and its frequency is untouched.

The offsets are a **warp**, baked beside the shelter. For each sum's dominant
wave a sweep in the direction it travels carries the lag: over water of depth
d it falls behind by `k(d) / k_deep − 1` metres a metre, `k(d)` from the
dispersion relation (held to 8 times shorter, a 40 m wave's 5 m in 10 cm of
water: shallower than that it has broken to nothing). Land carries the lag on
unchanged. The lag spreads sideways as the shelter's lee does, so where rows
that crossed different shallows meet the crests join rather than step — and
rows carrying more lag bend the crests a little toward the shallows on their
own. Past the terrain it fades out across the margin.

On a grid of 4 m, half the shelter's, in full floats: a running sum is smooth,
and a lag of hundreds of metres has to come back to the centimetre or short
chop would bend. Under 3 ms a bake for a 1 km terrain, on a worker, with the
shelter.

Both halves read each sum at its warped point, carry its slopes and fold terms
through the warp's Jacobian, read the simulated sea's textures warped at a mip
for the squeeze, and read the swash's arriving sea at the warped waterline.
A shortened wave keeps its sideways pinch in metres, so crests grow peakier
toward the break, as shoaling waves do.

## Shelter

Depth cannot calm a fjord: its water is deep right up to the walls. What
calms it is land between it and the open sea the waves come from. A terrain's
water is baked an **exposure** — how much of each sum of the sea reaches each
point past the land — and both halves scale each sum by it before shoaling and
breaking.

**The bake.** A grid over the terrain and a quarter of its size again on every
side, at 2 m where the terrain is small enough (at most 1024 samples a side).
A sample is land when any of the terrain's heights under it stands above the
water at rest, so a spit narrower than a cell still blocks; stakes, piers and
other meshes do not shelter, the water runs under them. Then, for each way a
sum arrives, a sweep down the grid in the direction its waves travel: open sea
at the upwind edge, nothing past land, and a lee that fills back in with
distance as waves bend around the end of the land that cast it, its edge
widening as the square root of wavelength times distance. The directions are
summed by how much of the sum arrives each way, and what is kept is the
amplitude share. Past the terrain it fades back to the open sea across the
margin, so a lee near the edge does not end in a line.

**Which ways a sum arrives.** Each Gerstner train one way, weighted by its
energy. The simulated sea across its whole spread, weighted by its own
spreading function: a storm sea arrives over a wide fan, so a cove turned
from the mean wind still takes waves from the side, and turns rough when the
storm swings round.

**Half the lee is kept** (`SHELTER_STRENGTH`, 0.5). Baked whole, a cove the
land turns from the sea keeps a fifth of the open sea's height or less and
reads as dead water; the test terrain's lagoon did. Kept at half, every lee is
clearly calmer than the open sea and the water in it alive. The bake itself is
physical, and its tests check it at full strength — the lee behind a
breakwater's end against the diffusion of a step.

**When it changes.** The land is marked again when the terrain is edited or
the water or terrain moves, on the calling thread, since that is where the
heights are edited. The exposure is baked on a worker, one at a time, when the
land or the sea's ways have changed — the sea's heading in degrees, its
wavelength in five-percent steps — and the last one is handed out meanwhile. A
body of water copied into play keeps the exposure it had until its own is
baked. As with a newly drawn sea, two machines adopt a new exposure a few
steps apart.

What it does not do:

- **Shallows do not shelter.** A reef that breaks the waves passing over it
  calms the water behind it; only land above the water counts here.
- **The sea in a lee is the open sea made smaller.** A cove's own wind raises a
  shorter, younger sea over its few hundred metres; one spectrum is drawn for
  the world, so that difference cannot be drawn.

## Swash

Breaking holds the waves to what the depth can carry, which is nothing at the
still waterline, so the waves alone would leave the water's edge standing
still. A beach does not: each broken wave runs on up the sand as a sheet and
drains back, and across the surf the mean level stands a little raised
(setup). Both are a **vertical lift** of the water level near the waterline.
Where the water's edge lands is the terrain's: a lift of R moves it R / slope
up the beach.

**What drives it.** Each point near the waterline reads the sea where its line
down the beach meets the still waterline — the sea's own height there, before
the shore scaled it, as a share of its significant amplitude. That share
swings the level of every point up and behind that spot, so the edge runs up
with the breakers that are drawn arriving there, in their sets, and needs no
arrival time worked out for it.

**How high.** From the sea's significant height after shelter H0, its
wavelength L0 and the beach slope β, after Stockdon et al. (2006):

- setup `0.35 β sqrt(H0 L0)`, all of it at and above the waterline, falling to
  nothing at the depth the sea breaks in;
- the level swings half of `sqrt(H0 L0 (0.563 β² + 0.004))` either side of
  it, all of it at and above the waterline, falling to nothing in water twice
  as deep as the swing.

β is held to 0.15 (`SWASH_STEEPEST_BEACH`): steeper ground is a rock face that
takes spray, not setup. On `coastal` with a 1:10 beach the edge moves a few
metres either way; on a gentler beach it travels further.

Sand higher than the swash can reach keeps the water under it, and nothing is
worked out there. The swash is vertical only, like a wake, so it changes no
fold measure; normals take its slope, which across a point is the lift's
change with depth plus the arriving sea's slope along the shore — every point
down one line of the beach reads the same spot.

## Foam in the surf

`BREAKING` in a water `fragment()` is the share of its height the sea has lost
to breaking here: 0 where the depth carries every wave, rising through the
surf to 1 at the waterline. It is what a surf zone foams on.

`CREST` cannot do it. It is the pinch of the surface, and breaking takes height
off faster than the shallows sharpen the waves, so crest foam peaks at the
break point and fades toward the beach while real surf is whitest there.

`SWASH` is how much of the swash a point carries: 1 at and above the still
waterline, fading to 0 seaward. The sheet's thinness over the sand is
`WATER_DEPTH_VERTICAL`, so a foamy run-up front is the two together.

## Evaluated twice

Like the waves, the shore is a pair that must agree. The bakes run once, on
the CPU (`water_shelter.cpp`), and their grids reach the GPU as the
`shore_exposure` and `shore_warp` textures; both halves read them the same
way:

- `shaders/include/fjell_water_shore.glsl` — what is drawn
- `src/renderer/resources/water_shore.cpp` — what physics floats on

`tests/test_water_shore.cpp` compiles the GLSL as C++ and compares the two
across a beach, and checks both against the physics: `Ks` against the
dispersion relation solved directly, the surface's normals and fold measure
against finite differences of where it puts points.

`Ks` is a fitted Chebyshev series rather than the physics solved each time:
the physics wants `tanh` and `sinh`, which the C libraries do not round alike,
and a point of the sea has to come out the same on every machine. It is within
1e-4 of the solved value.

The shader fetches the bed a sample at a time (`texelFetch`) and blends it
itself, so the blend is the CPU's rather than the texture unit's.

Scaling a sum by a factor that changes across the ground changes the surface's
slope as well as its height, so normals and the fold measure carry the
factor's gradient too. That makes the horizontal Jacobian asymmetric once the
water shallows, which is why it keeps both off-diagonal entries.
