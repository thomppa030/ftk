# Wind Response

A `.fjwater` can answer the wind the [weather](../Environment/Weather.md)
system is blowing: a front arrives and the sea rises under it, sharpens, and
combs around onto the new wind. Off by default, because most water is a pool, a
canal or a harbour and should sit exactly as it was authored.

Turn it on in the water editor under **Wind response**.

The response belongs to a **contributor**, not to the file. A `.fjwater` of
several contributors hands the same wind to each and lets each answer by its own
dials, which is how one sea holds a swell that rolls through unmoved — raised by
a storm somewhere else, response off — under a chop that rises and turns with
the weather overhead. Everything below describes one Gerstner contributor.

A simulated contributor answers the wind differently, and with one switch rather
than four dials: it is a description of the sea a wind raises, so answering the
weather is being raised by the weather's wind instead of its own. See
[Simulated Sea](Simulated-Sea.md).

## The authored sea is the sea at the reference wind

The wave list you tune is **not** the sea in a dead calm. It is the sea at the
**reference wind** — 8 m/s by default, a fresh breeze, whitecaps forming, which
is the condition most open water is authored at and the middle of the range a
sea spends its time in.

At that wind, nothing below changes anything: what you tuned is what you get.
Wind then moves the sea either side of it. Authoring the calm instead would
mean spending your time tuning a sea nobody ever sees.

## The dials

Each response is quoted **at twice the reference wind**, so the number means
something you can check rather than being an arbitrary gain.

**Height at double wind** scales amplitude. Real wave height grows steeply with
wind — roughly with its square in a fully developed sea — but a sea that
quadruples in a gust reads as a bug rather than as weather, so this is a
straight ramp and an artist's number rather than an oceanographer's. 1 leaves
height alone and lets wind steer direction only.

**Headroom at double wind** multiplies `steepness_headroom`. Wind does not only
raise a sea, it sharpens it: the same swell under a gale is pinched where under
a breeze it is round. It is a separate dial from height because it carries a
separate risk — a sea authored near its fold ceiling *will* fold in a storm.
The editor's wind table shows exactly where that happens, so you can decide
rather than discover.

**Turns onto wind** combs the trains around. Only the short ones turn: a long
swell was raised by a storm somewhere else and holds its direction for days
after the local wind has changed, while the chop riding on it answers the wind
it is actually in. The ramp runs by wavelength between the longest and shortest
train of the contributor, so a sea keeps its swell while the wind works the
surface.
One wind direction for the whole sea would turn it like a sheet of plywood.

Below the dials the editor shows what they come to at the winds that actually
blow — height, headroom and measured folding at nothing, half, reference, one
and a half, and double. A response quoted at double the reference is otherwise
a number about a sea you are not looking at.

## What it deliberately does not do

**It does not answer the gust.** `wind_gust` breathes on about a one-second
cycle. A sea driven by it swings its whole height that fast — measured at a
gale, between 0.93x and 2.27x, every second, the entire ocean heaving. Real
water does not work that way: wind takes minutes to hours to raise a sea, and
what a gust does meanwhile is ruffle the surface, which is the material's
business and not the wave field's. Water reads the sustained wind only.

**It does not track the wind, it follows it.** Water has mass. A weather event
steps its wind at a phase boundary and a field driven straight from it would
step the whole sea with it. So the wind the field is built from lags the wind
that is blowing, approaching it over about 25 seconds as it rises and 45 as it
falls — asymmetric because that is the way round it happens, a wind raising a
sea over hours and a dropped wind leaving a swell rolling for longer still.
Those are seconds rather than hours because a storm in a game passes in
minutes, and a sea answering on the real timescale would never visibly answer.

Jumping the weather to a phase — the weather editor, or a scene opening
mid-storm — resets that lag, because it is someone asking to see that moment
rather than weather arriving.

## Storms need no second asset

There is no blending between a calm `.fjwater` and a stormy one, and there
should not be: two wave lists' trains do not correspond, so interpolating train
3 into train 3 means nothing.

A `.fjweather` already animates `wind_speed` across its phases and crossfades
between events as a front passes. One field driven by that wind covers the
whole range continuously, and is the more correct shape besides.

## For programmers

`water::apply_wind()` is a derivation, never an edit. It reads the untouched
authored field and produces `WaterComponent::effective_water`, rebuilt each
frame by `EnvironmentSubsystem::update_water()`. The field's overload hands the
wind to each contributor's own; there is one per kind, and a kind without one
does not compile. The `.fjwater` on disk stays
what the artist tuned, so saving never bakes in whatever the weather happened
to be doing.

**Everything that asks where the water is reads `effective_water`** — the
renderer does, and buoyancy must — or a boat floats on the calm sea while the
player watches a storm.

The lag is `water::advance_sea_wind()`, kept as a pure function beside the
evaluator so it can be tested; it is frame-rate independent, and there is a
test that says so.
