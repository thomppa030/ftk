# Water (.fjwater)

A body of water is a **Water** component on a node, naming a `.fjwater` wave
field and a `shader_type water` material. The surface is generated in a mesh
shader from the field rather than stored as vertices, so a sea costs the same
whatever size you give it.

Where the water sits is the node's transform: its Y is the surface at rest, and
the waves move about it. Rotating the node does not turn the sea — a wave field
travels in world directions, and a rotated body of water is the same water.

## The wave field

A `.fjwater` is a list of **contributors**, and the surface is their sum. A
contributor is one source of motion with its own scales, fold headroom and wind
response; nothing downstream knows or cares how many a sea is made of, because
everything that asks — drawing, buoyancy, scripts — asks where the surface is.

The reason to use more than one is that parts of a sea want different answers.
A swell raised by a storm somewhere else holds its heading and its height
whatever the local wind does, while the chop riding on it answers the wind it is
in. As two contributors that is two wind responses; as one list of trains it is
a compromise.

There are two kinds. **Gerstner** is a list of up to 16 trains an author can
reason about one at a time, and a field sums at most four of them. **Simulated**
is a sea raised from a wind-driven spectrum on the GPU — alive where Gerstner is
art-directed — and has a page of its own: [Simulated Sea](Simulated-Sea.md). The
rest of this page is about Gerstner contributors.

```json
{
    "version": 3,
    "contributors": [
        {
            "kind": "gerstner",
            "amplitude_scale": 1.0,
            "speed_scale": 1.0,
            "steepness_headroom": 0.3,
            "generator": { "swell": 1.6, "wave_count": 5 },
            "waves": [
                { "direction": [1.0, 0.0], "wavelength": 120.0, "amplitude": 1.6,
                  "steepness": 0.45, "speed": 1.0 }
            ]
        }
    ]
}
```

A file written before contributors existed kept one sea's keys at its root. It
loads as a single Gerstner contributor at exactly the surface it was tuned to,
and is written back in the current shape the next time it is saved. A
contributor of a kind the engine does not know fails the load rather than being
skipped, since skipping it would save the file back without it.

Each train of a Gerstner contributor is a Gerstner wave:

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

The water editor's **Sea** panel lists the field's contributors. Select one and
the rest of the panel, and the **Waves** panel beside it, are that
contributor's: its generator, its scales and headroom, its wind response, its
trains. **Add Gerstner**, **Up**, **Down** and **Remove** work on the list;
order changes nothing about the sea, only how you keep it. The measures under
the list — crest to trough, longest period, shortest train — and the folding
percentage are always of the whole sea, because that is what gets drawn and
what folds.

`wavelength_falloff` is what separates a wind sea from a swell. Near 0.5 each
train is half the last, the long ones are quickly buried under chop, and the
surface reads as locally whipped up. Near 0.85 the trains stay close in size,
the long ones keep the surface, and what arrives is a procession of broad
ridges — water that has travelled from a storm somewhere else.

## Folding, and the headroom dial

A single Gerstner train turns inside out when its steepness term reaches 1, so
**steepness 1 is that train exactly at its own limit** and a lone train can
never fold whatever you set.

Trains *sum*, though, and the surface folds on the sum: several steep trains
whose crests meet can turn the surface through itself — dark flat facets on the
peaks, back-facing, exactly where the foam is.

That is **not prevented, it is measured.** The budget used to be shared 1/N,
which bounded the worst case but priced it wrongly: the worst case is every
crest aligned at one instant at one point, and trains travelling at different
speeds, angles and wavelengths do not arrange that. Designing to it held every
train on a six-train sea below an eighth of its own steepness, so the slider an
artist reaches for to get a pinched crest could not produce one — and adding a
small train for detail flattened every train already there.

So steepness now means what it says, and the editor tells you what your field
actually costs: it samples the surface over time and reports what percentage of
it folds.

**`steepness_headroom`** scales the whole field past a single train's limit. It
defaults to 0.30, which is just under the measured ceiling for the sea the
generator makes — nothing folds there, and 1.0 folds about 14% of it. A sea of
fewer or gentler trains can go far higher, which is why the dial is a
measurement rather than a rule.

Some folding is not a bug. A little of it at distance is often the trade worth
making, and it is what gives a sea the connected ridges that read as weather
rather than as a tiled pattern. Treat the percentage as information, not as an
error to drive to zero.

## Evaluated twice

The Gerstner contributors are evaluated in two places, and they must agree:

- `shaders/include/fjell_water_waves.glsl` — what you see
- `src/renderer/resources/water_surface.cpp` — what physics asks

That is the whole reason waves live in an asset rather than in GLSL: buoyancy
can ask where the surface is and get the answer the player is looking at. A
boat floating inside a wave is what drift between the two looks like.

`tests/test_fjwater.cpp` compiles the GLSL **as C++** through
`tests/glsl_shim.hpp` and compares the two at a grid of sample points, so drift
fails the build rather than shipping. It has caught every mismatch so far.
Change one half and you change the other, in the same shape.

The shader's evaluators read the field where it lives rather than being handed
it. GLSL passes a struct by value, and the field is over two kilobytes: given as
an argument it was copied out of the uniform buffer at every call, four times a
vertex, which measured at three times the cost of the whole water draw. So a
shader includes `fjell_water_field.glsl` for the layout, declares the buffer,
says where the field is with `#define FJELL_WATER_FIELD water.field`, and only
then includes `fjell_water_waves.glsl`.

A simulated contributor is not part of this pair. It is drawn once, on the CPU,
and what physics feels of it is that draw's strongest waves in closed form; the
GPU animates the whole of the same draw. See [Simulated Sea](Simulated-Sea.md).

Wakes are a second pair of the same kind — `fjell_water_wake.glsl` and
`water_wake.cpp`, pinned by `tests/test_water_wake.cpp` — summed on top of the
field for whatever has moved through the water. See [Wakes](Wakes.md).

The CPU side offers `height_at()` for the common case and
`height_at_iterative()` when a body must sit on the water actually above it —
Gerstner moves a vertex sideways, so the surface over a column started
somewhere else.

## Material

The look is a `shader_type water` `.fjmat` — colour, foam, refraction, depth
fade. The split is by *does the CPU need to know*: shape and motion are the
`.fjwater`, appearance is the material. Foam **colour** is material; foam
**amount**, from crest steepness, is derived from the wave field.

A placed body of water takes its material from its Water component, so one
wave field can back a calm lake and a storm-lit sea. The water editor picks
one of its own to preview through, stored in the `.fjwater` as
`preview_material` — an authoring setting beside `generator`, which nothing at
runtime reads. It is there because crests and foam read differently under
every material, and tuning a field against the engine's default before placing
it under the game's is tuning it blind. Unset previews with
`shaders/water_default.fjsl`, and a material whose shader is not
`shader_type water` is reported in the panel rather than silently drawing
nothing.

### The two depths

`fragment()` gets two, and they answer different questions:

| | |
|---|---|
| `WATER_DEPTH` | how far light travelled **through** the water to reach the eye — the view ray's length between the surface and whatever is behind it |
| `WATER_DEPTH_VERTICAL` | how deep the water is **straight down** |

Absorption wants the first: a shallow glance across a puddle tints more than a
look straight down, because the light went further. Beer's law is about path
length.

Anything asking *how close is land* wants the second — shore foam, a shallows
colour ramp, an alpha fade at the waterline. `WATER_DEPTH` cannot answer that,
because it changes as the camera moves: the same stretch of shore reads 0.3 m
from overhead and saturates at a standing eye height, which puts a foam band
everywhere when you look down at it and nowhere when you look across it.

Both are in metres. `WATER_DEPTH_VERTICAL` is large rather than zero over open
water — with no seabed to hit, the depth buffer reads the far plane — so a
shore term keyed on a small depth simply never fires out at sea.

### From below

`BELOW` is 1 when the eye is under the surface and sees its underside, decided
per pixel from the view direction against the surface normal, so an eye just
under a crest still sees the next trough from above. On the underside
`WORLD_NORMAL` points down at the eye, and the depths change meaning:
`WATER_DEPTH` is the water between the eye and the surface, and
`WATER_DEPTH_VERTICAL` is how far the surface stands above the eye. The look
is the material's: `water_default.fjsl` mirrors the water outside Snell's
window (about 48° from straight up, past which light cannot leave the water)
and shows the sky through it inside: the view ray refracted with `refract()`,
then `SCREEN_UV_TOWARD(dir)` says where that direction lands on the screen so
the full-resolution sky with its clouds comes from the screen copy, and the
rim of the window past the frame's edge from `sky_cubemap`. A material that
ignores `BELOW` shows its topside from underneath.

### Meeting solid things

`SDF_DISTANCE` is how far the surface is from solid geometry, in metres, read
from the scene SDF. A `smoothstep` on it is a foam collar against a cliff or a
pier, following the real geometry with nothing painted.

It sees **static geometry only** — the SDF is baked — so it answers for a cliff
and not for a passing boat. It is also clamped a few metres out by the bake, so
a large value means "nothing near" rather than a measured distance. Where the
scene has no SDF at all it reads far, and a collar keyed on a small distance
simply never appears.

See [Wind Response](Wind-Response.md) for how a sea answers the weather.
