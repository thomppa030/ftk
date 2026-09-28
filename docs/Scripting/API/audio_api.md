# fjell::audio

Plays audio clips at world positions.

| | |
|---|---|
| Header | `fjell/audio_api.hpp` |
| Source | `src/scripting/audio_api.cpp` |

## Syntax

```cpp
namespace fjell {
namespace audio {
    void play(const Asset<Audio>& clip,
              const glm::vec3& pos,
              float volume = 1.0f,
              float pitch  = 1.0f);
}
}
```

## Remarks

One-shot fire-and-forget. The clip plays once and self-destructs — there is no script-side handle to stop, pause, or query it. For persistent looping sounds attached to a node, use a scene-side `AudioComponent` (placed in the editor) instead.

The listener follows the active camera (see [`camera`](camera_api.md)). 3D attenuation is applied based on the source-to-listener distance.

Backed by miniaudio. Decoded synchronously on first play; subsequent plays of the same clip reuse the decoded buffer.

## Functions

| Name | Description |
|---|---|
| [`play`](#audioplay) | Play a one-shot clip at a world position |

---

## audio::play

### Syntax
```cpp
void play(const Asset<Audio>& clip,
          const glm::vec3& pos,
          float volume = 1.0f,
          float pitch  = 1.0f);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `clip` | `const Asset<Audio>&` | Audio asset reference. One that is unset or failed to load stops the game (see [`Asset`](Asset.md)). |
| `pos` | `const glm::vec3&` | World-space position the sound is played from. |
| `volume` | `float` | Linear volume multiplier. `1.0` = unattenuated, `0.0` = silent. Default `1.0`. |
| `pitch` | `float` | Playback rate / pitch. `1.0` = original, `2.0` = double-speed (one octave up), `0.5` = half. Default `1.0`. |

### Remarks
A clip left unset, or whose file failed to load, stops the game with the script and the field named. Where a sound is optional, check `if (clip)` first.

Volume above `1.0` is permitted and audible until clipping occurs in the mix. Extremely low pitches stretch the clip; extremely high pitches compress it — both alter perceived duration.

## Example

```cpp
class Coin : public fjell::ScriptComponent {
    FJELL_COMPONENT(Coin)

    [[fj::property]] fjell::Asset<fjell::Audio> collect_sound;

    void on_trigger_enter(uint32_t /*other*/) override {
        fjell::audio::play(collect_sound, transform().position);
        self().destroy();
    }
}
```

Randomized pitch for repeated sounds:

```cpp
float p = 0.95f + (rand() / float(RAND_MAX)) * 0.1f;  // 0.95..1.05
fjell::audio::play(footstep_sound, foot_pos, /*volume*/ 0.6f, p);
```

## See also
- [`Asset`](Asset.md) — how `Asset<Audio>` is resolved
- [`vfx`](vfx_api.md) — same `(asset, position)` shape for particle systems
