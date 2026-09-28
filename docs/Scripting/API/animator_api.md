# fjell::animator

Drives the skeletal `AnimatorComponent` on a node.

| | |
|---|---|
| Header | `fjell/animator_api.hpp` |
| Source | `src/scripting/animator_api.cpp` |

## Syntax

```cpp
namespace fjell {
namespace animator {
    void        set_param(const Ref<Node3D>& node, const char* name, float value);
    float       get_param(const Ref<Node3D>& node, const char* name);
    const char* current_state(const Ref<Node3D>& node);
}
}
```

## Remarks

These functions target the `AnimatorComponent` on `node`: `self()` for the script's own
node, or a [`Ref<Node3D>`](Ref.md) to another. A node that is gone stops the game. A node
with no `AnimatorComponent` is passed over: a setter does nothing and a getter returns its
default.

The component holds an `AnimationStateMachine` (loaded from a `.fjsm` asset); state
transitions are gated by *parameters* — floats with named keys. Bools and triggers are
floats too (`0.0` = false, non-zero = true).

A 2D sprite animation is driven on its `SpriteAnimComponent` directly, through its
`state_machine` (see `fjell/sprite_anim_component.hpp`).

See [State Machine](../../Animation/State-Machine.md) for authoring the state graph itself.

## Functions

| Name | Description |
|---|---|
| [`set_param`](#animatorset_param) | Write a parameter value |
| [`get_param`](#animatorget_param) | Read a parameter value |
| [`current_state`](#animatorcurrent_state) | Get the name of the currently playing state |

---

## animator::set_param

```cpp
void set_param(const Ref<Node3D>& node, const char* name, float value);
```

| Name | Type | Description |
|---|---|---|
| `node` | `const Ref<Node3D>&` | The node carrying the `AnimatorComponent`. |
| `name` | `const char*` | Parameter key, as spelled in the state machine. Must not be null. |
| `value` | `float` | New value. For bool/trigger params, `0.0` = false, non-zero = true. |

A name the state machine does not define is created rather than rejected, so a typo shows
up as an animation that never transitions.

Setting a parameter does not directly play an animation — it only updates the value.
Transitions are evaluated on the next animation tick and may or may not fire depending on
the state graph's conditions.

```cpp
// Drive a 2D blend space for locomotion
fjell::animator::set_param(self(), "forward", smoothed_input.y);
fjell::animator::set_param(self(), "strafe",  smoothed_input.x);

// Fire a jump transition (a "trigger" param)
fjell::animator::set_param(self(), "jump", 1.0f);
```

---

## animator::get_param

```cpp
float get_param(const Ref<Node3D>& node, const char* name);
```

The current value of the parameter, or `0.0f` if the node has no animator or the parameter
was never set. The two read the same; set a parameter before reading it if the difference
matters.

---

## animator::current_state

```cpp
const char* current_state(const Ref<Node3D>& node);
```

The name of the state playing now, or `""` if the node has no animator. During a
transition it is the *outgoing* state's name.

The string is owned by the engine and changes on the next transition: compare it at once,
and copy it to keep it.

## Example

```cpp
void tick(float dt) override {
    // Smooth raw input into blend-space params
    float t = 1.0f - std::exp(-8.0f * dt);
    smoothed_ = glm::mix(smoothed_, raw_, t);
    fjell::animator::set_param(self(), "forward", smoothed_.y);
    fjell::animator::set_param(self(), "strafe",  smoothed_.x);

    // React to landing
    const bool landing = std::strcmp(fjell::animator::current_state(self()), "Land") == 0;
    if (landing && !was_landing_) play_dust_vfx();
    was_landing_ = landing;
}
```

## See also

- [State Machine](../../Animation/State-Machine.md) — authoring state graphs and parameters
- [`AnimNotify`](AnimNotify.md) — animation-event callbacks (for events fired *from* the timeline)
