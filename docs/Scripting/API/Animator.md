# fjell::Animator

Drives the skeletal `AnimatorComponent` attached to a node.

| | |
|---|---|
| Header | `fjell/animator_api.hpp` |
| Source | `src/scripting/animator_api.cpp` |

## Syntax

```cpp
namespace fjell {
struct Animator {
    static void        set_param(uint32_t node_id, const char* name, float value);
    static float       get_param(uint32_t node_id, const char* name);
    static const char* current_state(uint32_t node_id);
};
}
```

## Remarks

These functions target the `AnimatorComponent` on the node identified by `node_id`. The component holds an `AnimationStateMachine` (loaded from a `.fjsm` asset); state transitions are gated by *parameters* — floats with named keys.

Bools and triggers are represented as floats (`0.0` = false, non-zero = true). The state machine asset defines which parameters exist; setting a parameter that doesn't exist is silently ignored.

See [State Machine](../../Animation/State-Machine.md) for authoring the state graph itself.

## Functions

| Name | Description |
|---|---|
| [`set_param`](#animatorset_param) | Write a parameter value |
| [`get_param`](#animatorget_param) | Read a parameter value |
| [`current_state`](#animatorcurrent_state) | Get the name of the currently playing state |

---

## Animator::set_param

### Syntax
```cpp
static void set_param(uint32_t node_id, const char* name, float value);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `node_id` | `uint32_t` | ID of a node carrying an `AnimatorComponent`. Typically `id()` from a `ScriptComponent` on the same node. |
| `name` | `const char*` | Parameter key, defined in the state machine asset. Must not be null. |
| `value` | `float` | New value. For bool/trigger params, `0.0` = false, non-zero = true. |

### Remarks
If `node_id` doesn't have an `AnimatorComponent`, the call is a no-op. If `name` isn't a defined parameter, the call is a no-op.

Setting a parameter does not directly play an animation — it only updates the value. Transitions are evaluated on the next animation tick and may or may not fire depending on the state graph's conditions.

### Example
```cpp
// Drive a 2D blend space for locomotion
fjell::Animator::set_param(id(), "forward", smoothed_input.y);
fjell::Animator::set_param(id(), "strafe",  smoothed_input.x);

// Fire a jump transition (a "trigger" param)
fjell::Animator::set_param(id(), "jump", 1.0f);
```

---

## Animator::get_param

### Syntax
```cpp
static float get_param(uint32_t node_id, const char* name);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `node_id` | `uint32_t` | Node carrying the `AnimatorComponent`. |
| `name` | `const char*` | Parameter key. Must not be null. |

### Return value
The current value of the parameter, or `0.0f` if the node has no animator or the parameter doesn't exist.

### Remarks
The return value doesn't disambiguate "not found" from "set to 0". If you need the distinction, set a sentinel parameter or check `current_state` instead.

---

## Animator::current_state

### Syntax
```cpp
static const char* current_state(uint32_t node_id);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `node_id` | `uint32_t` | Node carrying the `AnimatorComponent`. |

### Return value
The name of the currently active state, or `nullptr` if the node has no animator.

### Remarks
During a transition, returns the *outgoing* state's name. Use `AnimationStateMachine::is_transitioning()` (via direct access to the component) if you need the incoming state too.

The returned pointer is owned by the engine; do not free it. It remains valid until the next animation tick.

## Example

```cpp
void tick(float dt) override {
    // Smooth raw input into blend-space params
    float t = 1.0f - std::exp(-8.0f * dt);
    smoothed_ = glm::mix(smoothed_, raw_, t);
    fjell::Animator::set_param(id(), "forward", smoothed_.y);
    fjell::Animator::set_param(id(), "strafe",  smoothed_.x);

    // React to landing
    const char* state = fjell::Animator::current_state(id());
    if (state && std::strcmp(state, "Land") == 0 && !was_landing_) {
        play_dust_vfx();
    }
    was_landing_ = (state && std::strcmp(state, "Land") == 0);
}
```

## See also
- [State Machine](../../Animation/State-Machine.md) — authoring state graphs and parameters
- [`SpriteAnim`](SpriteAnim.md) — same shape for 2D sprite state machines
- [`AnimNotify`](AnimNotify.md) — animation-event callbacks (for events fired *from* the timeline)
