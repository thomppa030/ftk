# Simulated Sea

A `.fjwater` can hold a **simulated** contributor beside its Gerstner ones: a sea
raised from a wind-driven spectrum and carried forward on the GPU every frame.
Add one in the water editor with **Add Simulated**.

The two kinds are good at opposite things, which is why they sit side by side
rather than one replacing the other:

- **Gerstner is art-directed.** Six trains are something you reason about one at
  a time — "the swell comes from the north-west, big and slow". For a canal, a
  lake, a scripted storm beat, it is the better tool, not a fallback.
- **Simulated is alive.** Groups, crests that differ from one another and detail
  that never repeats fall out of a real spectrum for nothing. But it is a
  statistical description: you can say "this sea is choppy and from the west",
  not "a big one breaks *here*".

A simulated sea with one authored swell over it is how you get both: water that
is never the same twice, and the wave that rolls through on cue.

## What it is not

**It is drawn, not floated on.** Buoyancy, swimming and every script query that
asks where the surface is sum the Gerstner contributors and are blind to the
simulation. A boat rides the authored swell and ignores the simulated chop. That
is a deliberate first step with a known cost — a cork *should* ride chop a ship
ignores — so if the sea visibly heaves at a wavelength a floating body does not
answer, this is that decision showing, not a bug. Keep the waves a hull should
feel in a Gerstner contributor.

**The world runs one.** A simulation is a patch of sea stepped every frame, so a
field sums one simulated contributor and a scene runs the first one a water node
asks for.

## The dials

**The wind that raises it** — wind heading and speed, and **fetch**, how far that
wind has blown over open water. Together they decide how grown the sea is: a
short fetch is a lake's steep short chop, a long one the open ocean's heavy swell
under the same wind. The spectrum is JONSWAP, the measured shape of a sea still
growing under its wind.

**Answers the weather** replaces that wind with the one the
[weather](../Environment/Weather.md) is blowing, through the same lag an authored
sea answers by (see [Wind Response](Wind-Response.md)), so a front builds the sea
over its passage rather than at its arrival. More wind is a taller, longer sea
because that is what the spectrum says, not because a dial scaled it. A dead calm
leaves it flat. In the editor the **Preview wind** stands in for the weather.

**Stylisation**, because a correct ocean is busier than a readable one. A new
contributor opens stylised — height 2.9, choppiness 3, a 1.6 m high cut — since
the physical amounts (1, 1 and none) are correct and read as tame:

| | |
|---|---|
| **Height** | scales the whole simulated sea |
| **Choppiness** | how far crests are pinched. 1 is the physical amount and reads as gentle; past it crests sharpen until they fold and foam. The same motion a Gerstner train's steepness buys |
| **Spread** | the angle from the wind at which the sea carries half the energy it carries downwind. Narrow it for long, parallel, graphic ridges |
| **High cut** | fades out waves shorter than this. The biggest stylisation control: it turns a busy ocean into a few big readable swells |

**The simulated patch** — the side of the square that is simulated (it tiles
beyond that), the samples along it, and the seed. The patch wants several of the
longest waves in it or they are cut short, and few enough that the grid still
resolves the short ones; the editor says what the settings come to and warns when
the patch holds fewer than three dominant waves. Each step of resolution is four
times the cost. The same seed is the same sea on every machine, and it stays the
same sea when the wind changes: the waves grow and turn rather than being rolled
afresh.

## In a file

```json
{
    "kind": "fft",
    "wind_direction": [1.0, 0.0],
    "wind_speed": 9.0,
    "answers_weather": false,
    "fetch": 60000.0,
    "spread_degrees": 36.0,
    "high_cut": 1.6,
    "choppiness": 3.0,
    "amplitude_scale": 2.9,
    "patch_size": 256.0,
    "resolution": 256,
    "seed": 1
}
```

## For programmers

The spectrum is drawn on the CPU (`water_spectrum.cpp`): JONSWAP by frequency,
spread by direction, mapped onto the wavevector grid, times a deterministic
complex Gaussian per cell. `WaterFftPass` does everything per frame: it turns
every wavevector to the current time, runs a radix-2 inverse FFT over the height
spectrum and the two horizontal ones, and gathers a tiling **displacement**
texture and a **derivatives** texture, both mipped so a distant vertex reads the
sea averaged over the ground it stands for. It owns its images outside the
framegraph and steps once a frame however many viewports record it, the way the
fog density field does. At 256² it costs about 0.09 ms of GPU time.

`water.mesh` samples those textures into the same four sums the Gerstner trains
go into — offset, last frame's offset, the two tangents, the entries of the
horizontal Jacobian — and takes the normal and the fold measure afterwards. They
are kept as parts because the parts add across contributors and the finished
quantities do not.

Drawing a spectrum takes milliseconds, and a sea under the weather is a slightly
different one every frame. So the pass keeps the random part (it depends only on
the seed), lets a change of wind under 0.1 m/s and half a degree pass, and shapes
anything larger on the thread pool, swapping it in when it is done.

**Checking the transform.** Set `FJELL_VERIFY_WATER_FFT=1` and the pass simulates
a fixed 64×64 sea at a fixed time, reads the surface back, and holds it to a plain
Fourier sum of the same spectrum on the CPU, logging `VERIFIED` or
`VERIFICATION FAILED`. It agrees to under a millimetre. It pins time on purpose,
so it says nothing about whether the sea moves.

**The sign that is not the textbook's.** The horizontal displacement is
`+i (k/|k|) h`. The form usually printed has a minus, and under this engine's
Gerstner convention that spreads crests apart instead of pinching them.
`tests/test_water_spectrum.cpp` holds a spectrum of one wave to the Gerstner
evaluator for that wave, which shares no code with it.
