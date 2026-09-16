# fjell::World

Per-frame runtime state passed to game scripts. Plain struct — input, physics, and frame timings.

| | |
|---|---|
| Header | `fjell/world.hpp` |
| Source | (header-only) |

## Syntax

```cpp
namespace fjell {

struct World {
    const Input*  input_{nullptr};
    PhysicsWorld* physics_{nullptr};
    float         delta_time{0.0f};
    float         elapsed_time{0.0f};

    [[nodiscard]] const Input&  input() const;
    [[nodiscard]] PhysicsWorld* physics();
};

}
```

## Remarks

`World` is the catch-all "frame context" struct the engine populates before calling `tick()` on scripts. Components access it via [`ScriptComponent::world()`](ScriptComponent.md#scriptcomponentworld); widgets via [`UiWidget::world()`](UiWidget.md).

In practice you'll usually go through the convenience accessors on `ScriptComponent` / `UiWidget` (`input()`, `delta_time()`, `physics()`) which dereference `world()` for you. Touch `World` directly only when you need to pass per-frame state into helper functions.

`delta_time` is **already scaled** by [`Time::scale`](Time.md#timescale) and is `0` while paused. `elapsed_time` accumulates scaled time since the play session started.

## Members

| Name | Type | Description |
|---|---|---|
| `input_` | `const Input*` | Pointer to this frame's input state. Use `input()` instead. |
| `physics_` | `PhysicsWorld*` | Pointer to the physics world, or `nullptr` if physics is disabled. Use `physics()` instead. |
| `delta_time` | `float` | Scaled gameplay dt for this frame. |
| `elapsed_time` | `float` | Scaled gameplay seconds since the play session started. |

## Functions

| Name | Description |
|---|---|
| `input()` | Dereferences `input_`. Crashes if the engine left it null — should never happen in normal flow. |
| `physics()` | Returns the raw `physics_` pointer (may be null). |

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
