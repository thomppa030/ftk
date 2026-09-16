# Day Profiles (.fjday)

A day profile is the *look* of a day, keyed over 24 hours and reusable across
scenes. The [time of day](Time-of-Day.md) decides where the sun is; the profile
decides what colour it is when it gets there, how the sky and the fog change
around it, and how bright the world is exposed.

Set one on the World Environment's **Day Profile** field. Without one, the
scene looks exactly as its light and fog nodes author it, at every hour.

## Tracks

A profile is a set of named **tracks**, each a curve or a colour gradient keyed
over the 24 hours. A track the profile does not have leaves that value alone,
which is why an empty profile changes nothing and why you only author the
tracks you care about.

Each track has a **mode**:

- **Multiply** scales what the scene authors. A `sun_intensity` of 0.7 at dusk
  means seventy percent of whatever the light node is set to, so one profile
  works across scenes whose suns differ.
- **Override** replaces it outright. Sky colours use this, because there is no
  sensible thing to multiply.

The domain wraps: hour 24 leads back into hour 0, and a curve's last key
interpolates into its first across midnight rather than holding flat. You do
not need a key at both ends.

### The built-in tracks

| Track | Kind | Usual mode | What it shapes |
|---|---|---|---|
| `sun_color` | colour | multiply | The sun's light. Warm at dawn and dusk, neutral at noon. |
| `sun_intensity` | curve | multiply | How strong it is. Zero before sunrise and after sunset is normal, though the horizon fade already handles that. |
| `moon_color` | colour | multiply | The moon's light. |
| `moon_intensity` | curve | multiply | Its strength, on top of the phase. |
| `ambient_color` | colour | multiply | The ambient term's colour — the blue of open shade at midday, the warmth of it at sunset. |
| `ambient_intensity` | curve | multiply | How much ambient there is. This is what keeps night from being pitch black. |
| `sky_top` | colour | override | The built-in gradient sky's zenith. |
| `sky_bottom` | colour | override | Its horizon. |
| `fog_color` | colour | override | The volumetric fog's colour. |
| `fog_density` | curve | multiply | Its density. Dawn mist is a spike here. |
| `exposure_bias` | curve | override | EV added on top of the project's exposure. Negative at night is what makes night read as night rather than as a dimly lit day. |
| `star_intensity` | curve | override | The sky's `STARS` built-in. Overrides the automatic dusk fade. |

### Custom tracks

Any name the list above does not claim becomes a **sky shader uniform** of that
name, and is readable by scripts. This is the extension point: if your sky
shader declares `uniform float haze`, a `haze` track animates it over the day
with no engine change.

The sample profile uses this for `zenith_color` and `horizon_color`, which are
the gradient sky shader's own uniforms, so that sky animates over the day the
same way the built-in gradient does.

## The editor

Double-click a `.fjday` in the content browser.

- **Track list**, left. The built-in tracks it does not have yet are offered by
  name; below them, a field for a custom name and a kind. The `x` beside a
  track removes it. The selected track's mode is a combo below the list.
- **The curve or the gradient**, right. Float tracks get the curve editor with
  an adjustable value axis, so a `fog_density` that ranges 0 to 4 is not
  squashed into 0 to 1. Colour tracks get a gradient bar: click it to add a
  stop, drag a stop to move it, right-click to delete one, and the selected
  stop has a colour picker and an exact time.
- **The cursor**, a vertical line in the curve and the gradient alike, is the
  hour being previewed, labelled HH:MM.
- **The timeline**, on top, scrubs 0 to 24 and its transport plays the day. It
  drives the open scene's hour, so the preview and the scene move together.
- **The preview**, left of the tracks, renders the open scene under the profile
  at that hour, with its own sky rather than the editor's studio backdrop.

Every edit reaches the loaded asset immediately, so the preview and the scene
update as you drag rather than on save. **Ctrl+S** writes the file.

If the open scene does not use the profile you are editing, a **Use in scene**
button in the header sets it, so the preview shows what you are shaping. If the
scene's clock is off, the header says so, because a profile is read at noon
only until the clock runs.

## The file

Curves and gradients are keyed in **hours**, not normalized, so the file reads
the way the editor does.

```json
{
    "name": "default_day",
    "tracks": {
        "length": 24.0,
        "wrap": true,
        "tracks": [
            {
                "name": "sun_intensity",
                "type": "float",
                "mode": "multiply",
                "keys": [
                    {"t": 5.0,  "v": 0.0, "in": 0.0, "out": 0.0},
                    {"t": 12.0, "v": 1.0, "in": 0.0, "out": 0.0},
                    {"t": 19.0, "v": 0.0, "in": 0.0, "out": 0.0}
                ]
            },
            {
                "name": "sky_top",
                "type": "color",
                "mode": "override",
                "keys": [
                    {"t": 0.0,  "c": [0.02, 0.03, 0.08]},
                    {"t": 12.0, "c": [0.25, 0.5, 0.9]}
                ]
            }
        ]
    }
}
```

- `t` is the hour, `v` a float value, `c` an sRGB colour.
- `in` and `out` are the curve's tangents at that key, in value per hour.
- Colours in the file are **sRGB**, as the picker shows them; the engine
  converts to linear on the way in. This is the same rule as everywhere else in
  Fjell: files and pickers are sRGB, the runtime is linear.
- `length` and `wrap` are always 24 and true for a day profile; the loader
  forces them.

## The sample

`assets/environment/default_day.fjday` uses every built-in track and two custom
ones, as a worked example rather than a starting point to keep. Its shape:
warm low sun at 5:30 and 18:30, neutral at noon, ambient dropping to a sixth
overnight, the gradient sky moving from near-black through dawn orange to
midday blue, fog thickening at dawn, exposure a little under at night, moon and
stars fading in around dusk.

## From a script

Custom tracks are readable by name while a profile is bound:

```cpp
float haze = fjell::Weather::param("haze", 0.0f);
```

That call reads the *weather*'s custom tracks first; a day profile's custom
tracks reach sky shader uniforms rather than this call. To animate a gameplay
value over the day, a weather event's custom track or your own script reading
`TimeOfDay::hour()` is the more direct route.

## Things that will surprise you

- **A multiply track on a value the scene authors as zero stays zero.** If the
  sun's intensity is 0 on the light node, no `sun_intensity` curve will raise
  it. Multiply scales; it does not set.
- **Sky colour tracks only reach the built-in gradient sky.** With an FJSL sky
  shader set, `sky_top` and `sky_bottom` are not what that shader reads unless
  it happens to declare uniforms of those names. Drive a custom sky through
  custom tracks named after its own uniforms.
- **The editor's timeline drives the scene's hour.** That is deliberate, so the
  preview is truthful, but it means scrubbing the profile moves the scene's
  clock. Outside play that is an edit the scene will save.
- **Undo does not yet cover the track editor.** Curve and gradient edits are
  immediate and not on the undo stack. Ctrl+S when you are happy, and reopen
  the file to discard.

## See also

- [Time of Day](Time-of-Day.md) — the clock and the bodies the profile shapes
- [Weather](Weather.md) — events that layer over the profile and can bring their own
- [Sky Shaders](Sky-Shaders.md) — what custom tracks can reach
