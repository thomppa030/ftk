# Weather

Weather in Fjell is not a preset you fade between. A `.fjweather` is an
**event with a life of its own**: it approaches, sets in, holds, and tails off,
on curves the artist shapes, and one event fades into the next by playing its
tail underneath the newcomer's approach. A `.fjclimate` decides which event
comes next, so a scene keeps having weather without a script choosing each turn.

This page covers the event format, the climate that drives it, what the weather
does to the world, and how to author and drive it.

## The shape of an event

Four phases, in order:

| Phase | What it is |
|---|---|
| **Approach** | The front is coming. Wind rises, the sky thickens from windward, the light drops. Nothing is falling yet. |
| **Onset** | The front passes overhead. Precipitation ramps. |
| **Sustain** | The weather proper. May be indefinite, which is how a steady state like "clear" is expressed. |
| **Tail** | It tapers, the sky opens from the same side it closed from, and the ground stays wet behind it. |

Each phase has a **duration range**, and the player rolls a length from it, so
two storms of the same event are not the same length. The sustain can be marked
**indefinite**, in which case it holds until something else is played.

Tracks are keyed in **phase units**: 0 to 1 is the approach, 1 to 2 the onset,
2 to 3 the sustain, 3 to 4 the tail. A rolled duration stretches a phase
without moving the keys inside it, which is the point of the scheme — you
author "wind peaks just before the rain starts" once and it holds whether the
approach rolls sixty seconds or two minutes.

## Tracks

Like a [day profile](Day-Profiles.md), an event is a set of named tracks with a
multiply-or-override mode, and a track it lacks leaves that value alone.

| Track | Kind | Usual mode | What it shapes |
|---|---|---|---|
| `cloud_coverage` | curve | override | How much of the sky has cloud, 0 to 1. Drives the sky shader and the cloud shadows. |
| `cloud_density` | curve | override | How thick that cloud is. |
| `precipitation` | curve | override | How hard it falls, 0 to 1. Drives the particle rate, the audio bed and the wetting rate. |
| `wind_speed` | curve | override | Metres per second, before gusts. |
| `wind_gust` | curve | override | How much the wind breathes, 0 to 1. |
| `wind_veer` | curve | override | Degrees added to the wind's heading over the event, so a storm can swing round as it passes. |
| `fog.<uniform>` | as declared | multiply for density, else override | A uniform of the haze's [fog material](Fog-Shaders.md), on top of what the hour made of it; `fog.<group>.<uniform>` reaches the [fog volumes](Fog-Volumes.md) in that group. `fog_density` and `fog_color` are the older names for the density and colour. |
| `post.<uniform>` | as declared | override | A uniform of the infinite [post process volumes](../Post-Process.md), on top of what the hour made of it: a storm that drains the colour. `post.<group>.<uniform>` reaches the volumes in that group. |
| `sun_intensity` | curve | multiply | Dims the sun. |
| `ambient_intensity` | curve | multiply | Dims the ambient. |
| `sky_tint` | colour | multiply | Tints the sky shader's output through `SKY_TINT`. |
| `lightning_rate` | curve | override | Strikes per minute. Zero means none. |
| `sky_blend` | curve | — | How far to fade toward the event's own sky shader and day profile, when it brings them. |

Anything else is the event's own value: a sky uniform of that name, and a
number scripts read with `weather::param("name")`. Temperature, visibility,
danger — whatever the game needs keyed alongside the clouds.

## Event fields

Beside the tracks:

| Field | What it does |
|---|---|
| **Precipitation type** | None, rain or snow. Decides which VFX slot is used and whether wetness or snow accumulates. |
| **Wetness target** | How wet exposed surfaces get while it rains, 0 to 1. |
| **Snow target** | How covered they get while it snows. |
| **Wind heading** | Where the wind blows toward, in degrees about the up axis, 0 being +Z. The front comes from upwind of this. |
| **Sky shader** | Optional. A storm sky is often a different picture, not a tinted one; this fades in over the scene's by the `sky_blend` track. |
| **Day profile** | Optional, and blended the same way. |
| **Bolt / Rain / Snow / Splash** | VFX references. |
| **Bed / Wind / Thunder** | Audio references. |
| **Lightning** | Colour, intensity, radius, flash length, distance range and the flash curve. See [Lightning](#lightning). |

## Wetness and snow are integrated, not blended

This is the part that makes weather feel like weather, so it is worth stating
plainly: wetness and snow are **not** tracks. They accumulate.

While it rains, wetness rises toward the event's target at a rate. When the
rain stops, it decays at a different, slower rate. So a street stays dark and
glossy for minutes after a shower, and a brief drizzle never fully soaks
anything. Snow behaves the same way, and rain washes lying snow away faster
than it melts on its own.

The four rates are in **Project Settings → Environment**, in seconds to get
most of the way there:

| Rate | Default | Meaning |
|---|---|---|
| Wetting | 60 s | How fast rain soaks exposed surfaces |
| Drying | 600 s | How slowly they dry afterwards |
| Snowing | 300 s | How fast snow accumulates |
| Melting | 1200 s | How slowly it melts |

They are project-wide rather than per-event, because they describe the world's
materials rather than a particular storm.

## Fronts

Weather arrives from somewhere. Each event has a wind heading, and the front
travels with it.

During the **approach**, cloud coverage is a gradient across the sky whose edge
sweeps from the windward horizon toward the leeward one. Through onset and
sustain the sky is covered. In the **tail** the direction flips and the edge
sweeps back, so the sky opens from the same side it closed from.

Sky shaders read this as `FRONT_DIR`, `FRONT_EDGE` and `FRONT_SOFTNESS`, and
the built-in cloud layer uses it, so a sky that calls `cloud_layer()` gets the
arrival for free. The cloud shadow map uses the same edge, so the shadow of an
approaching front crosses the terrain toward you.

## What the weather does to the world

- **Surfaces.** Exposed, up-facing faces darken and go glossy as wetness rises,
  ripple while it is actually raining, and take a white, rough, noise-broken
  snow layer as snow accumulates. Shaders can opt out with
  `render_mode no_weather` and answer the weather themselves; see
  [Sky Shaders](Sky-Shaders.md#what-surfaces-see) for the built-ins.
- **Shelter.** A roof, a bridge or a canopy keeps the ground under it dry, and
  stops rain particles. Anything opaque counts, within 64 m of the camera and
  within 40 m above or below it. See [Shelter](#shelter).
- **Particles.** The rain or snow effect plays from a hidden emitter parked
  above the camera, its rate following how hard it falls.
- **Wind.** Particle emitters with **Use Global Wind**, `vertex()` shaders
  calling `wind_sway()`, rigid bodies with a **Wind Area**, and characters with
  a **Wind Push** all lean the same way, gusts included.
- **Audio.** The event's bed follows how hard it falls, its wind bed follows the
  wind. Both play in play mode only.
- **Light.** Cloud shadows dim the ground under cloud; an overcast sky has no
  hard shadows at all.

## Climates (.fjclimate)

A climate is a table of states. Each names an event, how likely it is, the
hours it may start in, and which states may follow it.

```json
{
    "name": "temperate",
    "seed": 1337,
    "initial": "clear",
    "states": [
        {"name": "clear",    "event": "assets/environment/clear.fjweather",
         "weight": 3.0, "next": ["clear", "overcast"]},
        {"name": "overcast", "event": "assets/environment/overcast.fjweather",
         "weight": 2.0, "next": ["clear", "overcast", "rain"]},
        {"name": "rain",     "event": "assets/environment/rain.fjweather",
         "weight": 1.0, "next": ["overcast"]}
    ]
}
```

- `weight` is relative among the states allowed at that moment; 0 never rolls.
- `next` restricts what may follow. Empty allows anything. This is what keeps
  clear from jumping straight to a blizzard.
- `from_hour` and `to_hour` limit when a state may *start*. A window may wrap
  midnight (22 to 4). Equal bounds mean any hour. Morning fog is the obvious
  use.
- `seed` decides the rolls.

Set one on the World Environment's **Climate** field. When an event ends, the
director rolls the next among those the current state allows at the current
hour, weighted. If nothing is allowed, whatever is playing holds rather than
the climate picking something it forbade.

Rolls are a pure function of the seed and how many events have played, so the
same seed replays the same weather exactly. The inspector shows what is coming
next, and **Hold** freezes the director without stopping what is playing.

## Authoring

### The weather editor

Double-click a `.fjweather`.

- **Event** section: the phase durations with the sustain's indefinite toggle,
  precipitation type and targets, wind heading, the sky and profile the event
  brings, lightning, and the VFX and audio references.
- **Tracks** section: the same track editor as the day profile, keyed over the
  four phases, with the cursor labelled by phase and percent.
- **Play in scene** starts the event over the open scene; the timeline's
  playhead then follows it and the preview shows it. The small arrow beside the
  button plays from a later phase, so you are not waiting out a two-minute
  approach to look at the sustain.
- Every edit reaches the loaded asset at once. **Ctrl+S** writes the file.

### From the World Environment

The node's **Weather** field is the event the scene opens on. Below it, a pick
list offers the project's `.fjweather` files, a line shows what is playing and
its phase, and **Stop** and **Replay** are there, Replay with the same
play-from-a-phase menu.

The weather belongs to the scene it plays over. Opening another scene starts
from that scene's own: its event in its start phase, dry ground, and nothing a
script or the weather editor played over the scene before.

## From a script

```cpp
#include "fjell/environment_api.hpp"

[[fj::property]] fjell::Asset<fjell::Weather> storm;
[[fj::property]] fjell::Asset<fjell::Climate> climate;

void begin_play() override {
    fjell::weather::set_climate(climate);
}

void on_weather_changed(fjell::Asset<fjell::Weather> event, fjell::WeatherPhase phase) override {
    if (event == storm && phase == fjell::WeatherPhase::Onset) {
        village_.take_cover();
    }
}

void tick(float) override {
    if (fjell::weather::wetness() > 0.5f) footsteps_.set_wet(true);
}
```

| Call | What it gives you |
|---|---|
| `weather::play(ref)` | Start an event now; what is playing tails out under it. |
| `weather::stop()` | Let the current event tail out with nothing to follow. |
| `weather::set_climate(ref)` | Hand the weather to a climate. |
| `weather::clear_climate()` | Take it away again: what plays runs out, nothing follows. |
| `weather::hold(bool)` / `is_held()` | Freeze the director. |
| `weather::forecast(i)` | The i-th event to come, as an `Asset<Weather>`; unset past the end. |
| `weather::current()` | The playing event, as an `Asset<Weather>`; unset when clear. |
| `weather::phase()` / `phase_progress()` | Where it is in its life. |
| `weather::precipitation()` / `precipitation_type()` | How hard, and what. |
| `weather::wetness()` / `snow()` | How wet and how covered the world is. |
| `weather::cloud_coverage()` | How much sky the cloud covers. |
| `weather::wind()` | Direction times speed, gusts included. |
| `weather::front_direction()` | Which way the front travels, on the ground plane. |
| `weather::param("name")` | A custom track's value. |
| `weather::set_sky_param(...)` / `clear_sky_param(...)` | Override a sky uniform. |
| `weather::strike(offset, intensity)` | Fire one lightning strike. |

The hook `on_weather_changed(event, phase)` fires during play when a new event
starts or the playing one changes phase, so nothing has to poll. `event` is an
`Asset<Weather>`, compared with the script's own with `==`.

## Lightning

How often it strikes is the `lightning_rate` track, so a storm builds and
passes. What a strike *is* belongs to the event:

| Field | What it does |
|---|---|
| **Flash shape** | A curve over the strike's own length. This is the important one: a real strike stutters — two quick pulses and a dimmer third — and that is what tells lightning apart from a light being switched on. With no curve it falls back to a single pulse. |
| **Flash Length** | How long one strike lasts, in seconds. |
| **Color** | What it throws. A cold-blue summer storm and an orange dust storm are different assets, not different code. |
| **Intensity / Radius** | The transient light at the strike. |
| **Distance** | How near and far strikes land from the listener. Biased toward the far end, so most are distant and a close one is an event. |
| **Bolt** | An optional VFX spawned at the strike. |

A strike flashes the sky and lights the clouds from inside through the
`LIGHTNING` built-in, lights the scene from a point light on the same curve,
spawns its bolt, and sends thunder that arrives delayed by distance at 343 m/s
with its volume falling off. The gap is what tells you how far the storm is.

**Strike now** in the weather editor fires one while the event plays.

## Shelter

The engine renders a small depth map straight down over a square that follows
the camera. Anything drawn into it shelters what is under it.

- **Extent**: 64 m square, 40 m above the camera to 40 m below.
- **What counts**: anything opaque. A scaled cube works; nothing special is
  needed. Transparent and additive materials do not shelter, which is right for
  a window and wrong for a tarp — give the tarp an opaque material.
- **What reads it**: `SKY_EXPOSURE` in lit shaders, so sheltered surfaces stay
  dry and snow-free, and the particle collision module's **Overhead** source,
  which ends a drop where shelter stands above it.
- **When it exists**: only while something is falling or the world is still
  wet. A clear scene pays nothing.

Shelter is judged straight down, not along the rain's slanted path, so under a
roof edge counts as fully sheltered even in a driving wind. That is the usual
simplification and it reads fine until the wind is extreme.

## The samples

In `assets/environment/`:

- `clear.fjweather` — a light breeze and a few clouds, indefinite sustain.
- `overcast.fjweather` — thick cloud, dimmer sun, thicker fog, a cool sky tint.
- `rain.fjweather` — the full shape: wind first, then the light drops and fog
  builds, then rain through onset and sustain with lightning, then a tail that
  leaves the ground wet. Uses the rain effect and an authored stuttering strike.
- `temperate.fjclimate` — clear to overcast to rain and back.

## Things that will surprise you

- **A new event does not cut the old one off.** It tails out underneath, which
  is what makes handoffs read well, and means two events are composited for a
  while. That is deliberate.
- **Wetness does not snap.** Playing an event from its sustain gives you rain
  at once but ground that darkens over the next seconds. The wetting rate is in
  Project Settings if you want it faster.
- **Weather sounds and particles are play-only.** The editor viewport shows the
  lighting, sky and surface response, but does not fill itself with rain or
  play the bed.
- **Every effect in a scene shares one particle budget** of 65536. An emitter
  that would take the scene past it is refused with a warning naming it; the
  scene keeps running. Rain and snow together reserve about 27 percent.
- **Wind moves nothing by default.** Rigid bodies need a Wind Area and
  characters a Wind Push; both default to zero.

## See also

- [Time of Day](Time-of-Day.md) — the clock the weather layers over
- [Day Profiles](Day-Profiles.md) — the look of the day underneath it
- [Sky Shaders](Sky-Shaders.md) — clouds, fronts and the weather built-ins
