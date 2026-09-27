# Time of Day

The scene's clock, the sun and the moon. Everything here is authored on the
**World Environment** node; one scene has one of them, and the first one found
is the one the renderer uses.

Nothing on this page happens until **Time of Day** is ticked on that node.
Without it the scene's directional light points where you aimed it and stays
there, exactly as it did before any of this existed.

## How the engine sees the environment

Worth knowing before the details, because it explains why some values are
greyed out and why the renderer never disagrees with itself.

Every frame, the `EnvironmentSubsystem` reads the scene's authoring nodes —
the directional light, the World Environment, the volumetric fog — and folds
them, the day profile and the playing weather into one resolved
`EnvironmentState`. Every consumer reads that one struct: the shadow cascades,
the sky, the lighting cubemap, the fog, the sun shafts, the GI. No pass looks
at a node.

The practical consequences:

- A value the environment drives is **not** a value you can also set by hand.
  The sun's direction is the clear case: with a time of day running, the
  subsystem writes it every frame, so the light's Direction field is greyed
  out and shows a note saying so.
- Things layer in a fixed order: what the nodes author, then the day profile
  for the hour, then the playing weather, then a script's overrides. Later
  layers win.
- One scene has one environment. A second World Environment node is ignored.

## The clock

| Field | What it does |
|---|---|
| **Time of Day** | The hour, 0 to 24. The slider shows it as HH:MM and scrubbing it updates the viewport as you drag. |
| **Day Length (min)** | How many real minutes a full 24 hours takes. 20 is the default. Zero freezes the clock at the current hour. |
| **Run in Editor** | Advance the hour outside play mode. Off by default, because a clock that runs while you work is a distraction. |

The hour advances from **game time**, so `Time::set_scale()` speeds it up and
`Time::pause()` stops it, which is the fastest way to watch a full day.

In play mode the clock always runs. Stopping play restores the hour along with
everything else in the scene, since play mode snapshots the scene and puts it
back, so an hour that ran for ten minutes of play does not leave your scene
authored at 3 a.m.

## The sun's path

The sun travels a circle. Three fields shape it, and they are deliberately the
terms an artist thinks in rather than a latitude and a date.

| Field | What it does |
|---|---|
| **Path Tilt** | How far the circle leans away from passing overhead. 0 puts the noon sun straight up; 90 keeps it circling the horizon. 40 is a temperate default. |
| **North** | Turns the whole path about the up axis, which is to say where the sun rises from. |
| **Noon Hour** | The hour the sun peaks. 12 unless you want a world whose day is offset. |

Without a tilt the sun rises due east at 6, is overhead at noon, sets due west
at 18 and is underfoot at midnight. Tilting leans the whole circle toward +Z
and lowers the noon sun by the tilt angle: at tilt 40 the noon sun stands 50
degrees up. Sunrise and sunset stay on the horizon and stay twelve hours
apart whatever the tilt, which is not how the real world works away from the
equator, but is what makes the control predictable.

A geographic model — a date, a latitude and a longitude — is listed as future
work. It would produce the same three outputs and slot in beside this one.

### What the light actually does

The scene's sun is its **first directional light**, the same rule the renderer
has always used to pick one. The subsystem writes that light's direction every
frame; its colour, intensity and ambient stay yours, and are what the day
profile multiplies.

A body's light fades out across the horizon rather than switching off, over a
band that matches the atmosphere shader's own, so dusk is a fade in the
lighting and in the sky at the same rate. Below that band the sun contributes
nothing and the shadow pass builds no cascades from it.

## The moon

The moon rides the same circle as the sun, and its phase is what places it
there. The two are one value because they are one fact: the phase *is* the
angle between the bodies, and the disc is shaded by where the sun actually is,
so a position set separately would light a shape the phase disagreed with.

| Field | What it does |
|---|---|
| **Phase** | Where the moon sits on the sun's path, and so how much of it is lit. 0 and 1 are new (with the sun, unlit), 0.25 and 0.75 are the half-lit quarters, 0.5 is full — opposite the sun, highest at midnight. Its light scales with this, so a new moon casts nothing. |
| **Color** | The moon's own light. Cool blue by default. |
| **Intensity** | Its light at full phase, before the day profile's moon tracks. Small — 0.08 by default, against a sun at 1.5 — because moonlight is a hint, not an alternative sun. |

### Which body casts the shadows

Only one body casts the cascaded shadows. The brighter of the two above the
horizon takes the role, and the other is still lit — as a second directional
light without cascades, occluded by contact shadows only, which is enough for
a dim moon at dusk.

The handoff has hysteresis: the challenger must be clearly brighter, not
merely brighter, so twilight does not flip the shadow caster back and forth
from frame to frame.

In practice: the sun casts all day, the moon takes over once the sun has set,
and a new moon leaves the night with no cast shadows at all, which is correct.

## Stars

Stars fade in on their own once the sun is about nine degrees below the
horizon, and out again at dawn. A day profile's `star_intensity` track
overrides that if you want them shaped differently — brighter, or lingering.

They are drawn by the sky shader, so a sky that never calls `star_field()` has
none. They are fixed to the celestial sphere and wheel with the same rotation
that carries the sun, so they turn through the night rather than sitting
still.

Rotating rigidly is correct — that is what a celestial sphere does — but it is
not enough on its own: a field of identical unblinking dots slid across the sky
reads as a texture being dragged. So the stars also twinkle, harder near the
horizon where the line of sight crosses more air and almost not at all
overhead; they follow a lopsided brightness distribution, a few bright among
very many faint; they take their colour from the blackbody locus, with the
brightest spreading furthest toward orange and blue; and they cluster, with
voids, crowds and a galactic band carrying its own faint glow. See
[Sky-Shaders](Sky-Shaders.md#bodies) for the details.

## From a script

```cpp
#include "fjell/environment_api.hpp"

void tick(float dt) override {
    if (fjell::time_of_day::phase() == fjell::TimePhase::Night) {
        lantern_.light();
    }
}
```

| Call | What it gives you |
|---|---|
| `time_of_day::hour()` / `set_hour(h)` | The hour, 0 to 24. Setting it wraps, so 25 is 1 a.m. |
| `time_of_day::day_length()` / `set_day_length(s)` | Real seconds per day. 0 freezes it. |
| `time_of_day::phase()` | `Night`, `Dawn`, `Day` or `Dusk`. Twilight is within six degrees of the horizon; dawn before noon, dusk after. |
| `time_of_day::is_night()` | Shorthand for the phase being `Night`. |
| `time_of_day::sun_elevation()` | Radians above the horizon, negative once set. |
| `time_of_day::sun_direction()` | Unit vector from the ground toward the sun. |

There is also a hook, so nothing has to poll:

```cpp
void on_time_phase_changed(fjell::TimePhase phase) override {
    if (phase == fjell::TimePhase::Dusk) shopkeeper_.close_up();
}
```

It fires only during play and only on an actual change.

These calls write the World Environment node, so a change made outside play
shows in the editor and saves with the scene. A scene whose World Environment
has no time of day enabled sits at noon and the setters do nothing.

## Things that will surprise you

- **The light's Direction is greyed out.** That is the time of day driving it.
  Untick Time of Day to aim the sun by hand again.
- **A moved sun does not change the sky at once, everywhere.** The sky and the
  direct light follow immediately. Global illumination re-traces over several
  frames, so bounce light lags a fast day. That lag is inherent to the GI's
  own update rate, and an unusually fast day — a full cycle in under a minute —
  will show it.
- **Stars need a sky shader that draws them.** The built-in gradient and the
  procedural atmosphere do not.
- **A new moon means no shadows at night.** Working as intended; raise the
  phase toward 0.5 if you want moonlit shadows.

## See also

- [Day Profiles](Day-Profiles.md) — shaping the look over the 24 hours
- [Sky Shaders](Sky-Shaders.md) — what the sky can draw, and how it lights the world
- [Weather](Weather.md) — events that layer on top of all of this
