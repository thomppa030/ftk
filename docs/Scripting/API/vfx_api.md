# fjell::vfx

Spawns particle systems at world positions.

| | |
|---|---|
| Header | `fjell/vfx_api.hpp` |
| Source | `src/scripting/vfx_api.cpp` |

## Syntax

```cpp
namespace fjell {
namespace vfx {
    void spawn(const Asset<Vfx>& effect, const glm::vec3& pos);
}
}
```

## Remarks

One-shot. The effect runs its authored lifetime, then is destroyed. Looping or owner-tracked effects belong on a scene-side `VfxComponent` placed in the editor, not in script-spawned calls.

Spawn position is the *initial* world position of the system; the system itself remains at that location unless its modules move it. To follow a moving target, attach a `VfxComponent` to the target node.

See [VFX](../../VFX/Particle-System.md) for the authoring side.

## Functions

| Name | Description |
|---|---|
| [`spawn`](#vfxspawn) | Spawn an effect at a world position |

---

## vfx::spawn

### Syntax
```cpp
void spawn(const Asset<Vfx>& effect, const glm::vec3& pos);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `effect` | `const Asset<Vfx>&` | Effect to spawn. If null, the call is a no-op. |
| `pos` | `const glm::vec3&` | World-space spawn position. |

### Remarks
No return value — there's no script-side handle to the running system. To control an effect after spawn (stop it, change its rate, follow a moving target), use a `VfxComponent` instead.

## Example

```cpp
class Coin : public fjell::ScriptComponent {
    FJELL_COMPONENT(Coin)

    [[fj::property]] fjell::Asset<fjell::Vfx>   collect_effect;
    [[fj::property]] fjell::Asset<fjell::Audio> collect_sound;

    void on_trigger_enter(uint32_t /*other*/) override {
        fjell::vfx::spawn(collect_effect, transform().position);
        fjell::audio::play(collect_sound, transform().position);
        self().destroy();
    }
}
```

## See also
- [`audio`](audio_api.md) — same `(asset, position)` call shape
- [`Asset`](Asset.md) — how `Asset<Vfx>` is resolved
- [VFX Particle System](../../VFX/Particle-System.md) — authoring `.fjvfx` effects
