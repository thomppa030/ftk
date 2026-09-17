# Water (.fjwater)

A body of water is a **Water** component on a node, naming a `.fjwater` wave
field and a `shader_type water` material. The surface is generated in a mesh
shader from the field rather than stored as vertices, so a sea costs the same
whatever size you give it.

Where the water sits is the node's transform: its Y is the surface at rest, and
the waves move about it. Rotating the node does not turn the sea — a wave field
travels in world directions, and a rotated body of water is the same water.

## The wave field

A `.fjwater` is a list of **trains**, each a Gerstner wave:

| | |
|---|---|
| **Direction** | where it travels, on the ground plane |
| **Wavelength** | crest to crest, in metres |
| **Amplitude** | crest height above the still surface |
| **Steepness** | how far it pinches its crest, 0 (a sine) to 1 |
| **Speed** | multiplier on its natural deep-water speed |

Gerstner rather than a sum of sines because it displaces *horizontally* as well
as vertically, bunching vertices toward the crest. That pinches peaks and
broadens troughs, which is the silhouette stylized water reads as; sines give
rolling hills. The horizontal term is also what makes crest steepness
computable, which is where whitecaps come from.

A sea is usually five or six trains. The editor's **generator** fills the list
from a wind direction, a swell height and a falloff, and the list is then yours
to edit — generate, then tune.

`wavelength_falloff` is what separates a wind sea from a swell. Near 0.5 each
train is half the last, the long ones are quickly buried under chop, and the
surface reads as locally whipped up. Near 0.85 the trains stay close in size,
the long ones keep the surface, and what arrives is a procession of broad
ridges — water that has travelled from a storm somewhere else.

## Folding, and the headroom dial

A single Gerstner train turns inside out when its steepness term reaches 1.
Trains *sum*, though, and the surface folds on the sum: six trains each at a
safe 0.5 add to 3, and where their crests line up the surface turns through
itself — dark flat facets on the peaks, back-facing, exactly where the foam is.

So the budget is shared. With N trains each may take 1/N of the limit, and the
worst case where every crest aligns lands exactly on flat rather than past it.

That worst case assumes every crest aligned, at one instant, at one point —
which trains travelling at different speeds and angles never actually manage.
Holding a sea to it costs the pinch that makes a crest read as relief rather
than a bump. **`steepness_headroom`** is how far past it you are willing to go,
and the editor measures what your setting actually costs: it samples the
surface over time and tells you what percentage of it folds.

Some folding is not a bug. A little of it at distance is often the trade worth
making, and it is what gives a sea the connected ridges that read as weather
rather than as a tiled pattern. Treat the percentage as information, not as an
error to drive to zero.

## Evaluated twice

The field is evaluated in two places, and they must agree:

- `shaders/include/fjell_water_waves.glsl` — what you see
- `src/renderer/resources/water_surface.cpp` — what physics asks

That is the whole reason waves live in an asset rather than in GLSL: buoyancy
can ask where the surface is and get the answer the player is looking at. A
boat floating inside a wave is what drift between the two looks like.

`tests/test_fjwater.cpp` compiles the GLSL **as C++** through
`tests/glsl_shim.hpp` and compares the two at a grid of sample points, so drift
fails the build rather than shipping. It has caught every mismatch so far.
Change one half and you change the other, in the same shape.

The CPU side offers `height_at()` for the common case and
`height_at_iterative()` when a body must sit on the water actually above it —
Gerstner moves a vertex sideways, so the surface over a column started
somewhere else.

## Material

The look is a `shader_type water` `.fjmat` — colour, foam, refraction, depth
fade. The split is by *does the CPU need to know*: shape and motion are the
`.fjwater`, appearance is the material. Foam **colour** is material; foam
**amount**, from crest steepness, is derived from the wave field.

See [Wind Response](Wind-Response.md) for how a sea answers the weather.
