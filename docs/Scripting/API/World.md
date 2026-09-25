# fjell::World

Per-frame runtime state passed to game scripts. Plain struct — input, frame timings and the fixed step's time.

| | |
|---|---|
| Header | `fjell/world.hpp` |
| Source | (header-only) |

## Syntax

```cpp
namespace fjell {

struct World {
    const Input* input_{nullptr};
    float        delta_time{0.0f};
    float        elapsed_time{0.0f};
    float        fixed_delta_time{0.0f};
    double       fixed_time{0.0};
    uint64_t     fixed_step_index{0};

    [[nodiscard]] const Input& input() const;
};

}
```

## Remarks

`World` is the catch-all "frame context" struct the engine populates before calling `tick()` on scripts. Components access it via [`ScriptComponent::world()`](ScriptComponent.md#scriptcomponentworld); widgets via [`UiWidget::world()`](UiWidget.md).

In practice you'll usually go through the convenience accessors on `ScriptComponent` / `UiWidget` (`input()`, `delta_time()`, `elapsed_time()`) which dereference `world()` for you. Touch `World` directly only when you need to pass per-frame state into helper functions.

`delta_time` is **already scaled** by [`Time::scale`](Time.md#timescale) and is `0` while paused. `elapsed_time` accumulates scaled time since the play session started.

Inside [`fixed_tick`](ScriptComponent.md#scriptcomponentfixed_tick), `delta_time` and `elapsed_time` are the step's own length and time, so a helper that reads them is right whichever hook calls it. The `fixed_` members describe the simulation's latest step from anywhere.

## Members

| Name | Type | Description |
|---|---|---|
| `input_` | `const Input*` | Pointer to this frame's input state. Use `input()` instead. |
| `delta_time` | `float` | Scaled gameplay dt for this frame; the step's length inside `fixed_tick`. |
| `elapsed_time` | `float` | Scaled gameplay seconds since the play session started; the step's time inside `fixed_tick`. |
| `fixed_delta_time` | `float` | The simulation's step length in seconds, the same for every step. |
| `fixed_time` | `double` | Simulation time at the start of the latest fixed step. |
| `fixed_step_index` | `uint64_t` | How many fixed steps the session took before the latest one. |

## Functions

| Name | Description |
|---|---|
| `input()` | Dereferences `input_`. Crashes if the engine left it null — should never happen in normal flow. |

## Example

Component accessing world directly to pass into a helper:

```cpp
void update_enemy(fjell::World* w, Enemy& e) {
    e.move(w->delta_time);
    if (w->input().is_key_pressed(FJELL_KEY_F)) e.flee();
}

void tick(float /*dt*/) override {
    update_enemy(world(), enemy_state_);
}
```

## See also
- [`ScriptComponent`](ScriptComponent.md) — has `world()` and per-field convenience getters
- [`Input`](Input.md)
- [`Time`](Time.md) — controls `delta_time` scaling
