# fjell::sprite_anim

Drives the 2D `SpriteAnimComponent` attached to a node.

| | |
|---|---|
| Header | `fjell/sprite_anim_api.hpp` |
| Source | `src/scripting/sprite_anim_api.cpp` |

## Syntax

```cpp
namespace fjell {
namespace sprite_anim {
    void        set_param     (uint32_t node_id, const char* name, float value);
    float       get_param     (uint32_t node_id, const char* name);
    const char* current_state (uint32_t node_id);
    void        set_flip_h    (uint32_t node_id, bool flip);
    bool        is_finished   (uint32_t node_id);
    void        set_playing   (uint32_t node_id, bool playing);
}
}
```

## Remarks

Parallel to [`animator`](animator_api.md) but targets `SpriteAnimComponent` instead of the skeletal animator. The state machine drives which frame range plays from a `SpriteSheet`. State graphs use the same parameter model (named floats) as the skeletal animator.

Extra controls vs the skeletal version: horizontal flip, play/pause, and a finished query for non-looping states.

## Functions

| Name | Description |
|---|---|
| [`set_param`](#spriteanimset_param-get_param-current_state) | Write a parameter value |
| [`get_param`](#spriteanimset_param-get_param-current_state) | Read a parameter value |
| [`current_state`](#spriteanimset_param-get_param-current_state) | Get the active state's name |
| [`set_flip_h`](#spriteanimset_flip_h) | Mirror the sprite horizontally |
| [`is_finished`](#spriteanimis_finished) | Query whether a non-looping state has reached its end |
| [`set_playing`](#spriteanimset_playing) | Pause / resume sprite playback |

---

## sprite_anim::set_param / get_param / current_state

Identical semantics to the skeletal [`animator`](animator_api.md) versions. See those for parameter / state behavior.

```cpp
void        set_param(uint32_t node_id, const char* name, float value);
float       get_param(uint32_t node_id, const char* name);
const char* current_state(uint32_t node_id);
```

---

## sprite_anim::set_flip_h

### Syntax
```cpp
void set_flip_h(uint32_t node_id, bool flip);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `node_id` | `uint32_t` | Node carrying the `SpriteAnimComponent`. |
| `flip` | `bool` | `true` to mirror the sprite horizontally. |

### Remarks
Operates on the sprite, not the node transform — the node's rotation is untouched. Useful for "character faces movement direction" without authoring left-facing animations separately.

### Example
```cpp
fjell::sprite_anim::set_flip_h(id(), velocity_.x < 0.0f);
```

---

## sprite_anim::is_finished

### Syntax
```cpp
[[nodiscard]] bool is_finished(uint32_t node_id);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `node_id` | `uint32_t` | Node carrying the `SpriteAnimComponent`. |

### Return value
`true` if the current state is non-looping and has played past its last frame. `false` for looping states or while still playing.

### Remarks
Common pattern: poll this on a death / attack state to decide when to advance to the next state.

### Example
```cpp
if (fjell::sprite_anim::is_finished(id())) {
    self().destroy();   // death anim done, remove sprite
}
```

---

## sprite_anim::set_playing

### Syntax
```cpp
void set_playing(uint32_t node_id, bool playing);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `node_id` | `uint32_t` | Node carrying the `SpriteAnimComponent`. |
| `playing` | `bool` | `true` resumes playback, `false` pauses on the current frame. |

### Remarks
Pause is per-component — unlike [`Time::pause()`](Time.md#timepause), only this sprite stops. Use to freeze a sprite for a cinematic or "hit-stop" effect without affecting the rest of the game.

## Example

```cpp
class Enemy : public fjell::ScriptComponent {
    FJELL_COMPONENT(Enemy)

    void tick(float /*dt*/) override {
        // Face the direction of movement
        fjell::sprite_anim::set_flip_h(id(), velocity_.x < 0);

        // Death: play once, then despawn
        if (hp_ <= 0) {
            fjell::sprite_anim::set_param(id(), "dead", 1.0f);
            if (fjell::sprite_anim::is_finished(id())) {
                self().destroy();
            }
        }
    }

    glm::vec2 velocity_{};
    int hp_{100};
}
```

## See also
- [`animator`](animator_api.md) — skeletal counterpart
- [State Machine](../../Animation/State-Machine.md)
