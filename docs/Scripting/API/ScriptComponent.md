# fjell::ScriptComponent

Base class for game components attached to scene nodes. Aliased as `fjell::Component`.

| | |
|---|---|
| Header | `fjell/component.hpp` |
| Source | (header-only base; concrete subclasses live in your game project) |

## Syntax

```cpp
namespace fjell {

class ScriptComponent {
public:
    virtual ~ScriptComponent() = default;

    // Lifecycle (override these in subclasses)
    virtual void begin_play();
    virtual void tick(float dt);
    virtual void fixed_tick(const FixedStep& step);
    virtual void late_tick(float dt);
    virtual void end_play();

    // Collision hooks
    virtual void on_collision_enter(uint32_t other_id,
                                    const glm::vec3& point,
                                    const glm::vec3& normal);
    virtual void on_collision_exit (uint32_t other_id);
    virtual void on_trigger_enter  (uint32_t other_id);
    virtual void on_trigger_exit   (uint32_t other_id);

    // Identity
    [[nodiscard]] uint32_t    id()   const;   // node ID
    [[nodiscard]] const char* name() const;
    [[nodiscard]] Node3D      self() const;

    // Owning node's transform
    [[nodiscard]] Transform&       transform();
    [[nodiscard]] const Transform& transform() const;

    // World state
    [[nodiscard]] const Input&      input()        const;
    [[nodiscard]] float             delta_time()   const;
    [[nodiscard]] float             elapsed_time() const;
    [[nodiscard]] World*            world();
    [[nodiscard]] const World*      world() const;
};

using Component = ScriptComponent;

}
```

## Remarks

Subclass to add behavior to a scene node. Each component instance has its own owning node and transform; multiple components can live on the same node.

The registration macro `FJELL_COMPONENT(MyType)` makes the engine aware of the subclass — without it, the type doesn't show in the editor's "Add Component" menu and won't deserialize from `.fjscene`.

Reflected fields are declared with `[[fj::property]]` and processed by the `fjreflect` codegen. See [Overview — Reflection](../Overview.md#reflection-fjproperty-fjbind-fjaction).

### Lifecycle order

1. **Construction** — default constructor only. Don't access `transform()`, `input()`, etc. here; the engine hasn't wired them up.
2. **Field deserialization** — `[[fj::property]]` fields are written from `.fjscene` data.
3. **`begin_play()`** — engine has wired `world_`, `transform_`, `id_`. All members are valid. Bind input actions, set up timers, query other components, etc.
4. **Each frame while the scene is playing:** `tick(dt)`, then
   `fixed_tick(step)` once per fixed step the frame runs (often none or
   several), then `late_tick(dt)`. See
   [the three ticks](../Overview.md#the-three-ticks).
5. **`end_play()`** — exactly once. Cancel timers, clear delegate connections, destroy viewports. Members are still valid; you can still call engine APIs.
6. **Destruction**.

### Inherited members

`id_`, `name_`, and a few internals are public for engine access. **Do not write to them** from game code — they're populated by the engine before `begin_play()` and treating them as read-only is the contract.

## Lifecycle functions

| Name | Description |
|---|---|
| [`begin_play`](#scriptcomponentbegin_play) | One-time init after the engine wires everything up |
| [`tick`](#scriptcomponenttick) | Per-frame update, before the simulation steps |
| [`fixed_tick`](#scriptcomponentfixed_tick) | Per simulation step, before physics moves anything |
| [`late_tick`](#scriptcomponentlate_tick) | Per-frame update, after the simulation has stepped |
| [`end_play`](#scriptcomponentend_play) | One-time teardown before destruction |

## Collision functions

| Name | Description |
|---|---|
| [`on_collision_enter`](#scriptcomponenton_collision_enter) | Physical contact begins |
| [`on_collision_exit`](#scriptcomponenton_collision_exit) | Physical contact ends |
| [`on_trigger_enter`](#scriptcomponenton_trigger_enter) | A non-physical trigger volume is entered |
| [`on_trigger_exit`](#scriptcomponenton_trigger_exit) | A non-physical trigger volume is exited |

## Accessors

| Name | Description |
|---|---|
| [`id`](#scriptcomponentid) | Owning node's ID |
| [`name`](#scriptcomponentname) | Owning node's name |
| [`self`](#scriptcomponentself) | `Node3D` handle for the owning node |
| [`transform`](#scriptcomponenttransform) | Owning node's transform |
| [`input`](#scriptcomponentinput) | Frame's input state |
| [`delta_time`](#scriptcomponentdelta_time) | Scaled gameplay dt this frame, or the step's inside `fixed_tick` |
| [`elapsed_time`](#scriptcomponentelapsed_time) | Scaled gameplay time since session start |
| [`world`](#scriptcomponentworld) | Full `World` struct |

---

## ScriptComponent::begin_play

### Syntax
```cpp
virtual void begin_play();
```

### Remarks
Called once after the component is fully wired and field values are loaded. Override and do not call the base — there's nothing in the base body to chain.

By the time `begin_play` runs:
- `transform()`, `input()` and `world()` are valid.
- `[[fj::property]]` fields hold their serialized values.
- Other components on the same node may or may not have had `begin_play` called yet — don't assume order.

### Example
```cpp
void begin_play() override {
    fjell::input::bind<&Player::on_jump>(
        jump_action, fjell::EInputTrigger::Pressed, this);
    timer_ = fjell::Timer::set_repeating<&Player::regen>(1.0f, this);
}
```

---

## ScriptComponent::tick

### Syntax
```cpp
virtual void tick(float dt);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `dt` | `float` | Seconds since the last frame, scaled by [`Time::scale`](Time.md#timescale). 0 while [`Time::is_paused()`](Time.md#timeis_paused). |

### Remarks
Called every frame while the scene is playing, before the simulation's fixed
steps: react to input and decide what things should do here, and this frame's
steps see it. Motion the simulation owns — a physics body, a character moved
by `Velocity` — belongs in [`fixed_tick`](#scriptcomponentfixed_tick); the
editor reports a `tick` that moves a dynamic body or writes such a velocity.

`dt` matches `delta_time()` and `world().delta_time` — they're all the same value.

### Example
```cpp
void tick(float dt) override {
    // A node nothing simulates: turn it by the frame's time.
    transform().rotation = glm::rotate(transform().rotation, spin_ * dt, glm::vec3{0, 1, 0});
}
```

---

## ScriptComponent::fixed_tick

### Syntax
```cpp
virtual void fixed_tick(const FixedStep& step);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `step` | `const FixedStep&` | The step: `dt` (its length, the same for every step), `time` (simulation time at its start), `index`, and `input`, the input this step sees. From `fjell/fixed_step.hpp`. |

### Remarks
Called once per fixed step of the simulation while the scene is playing,
before physics moves anything: none, one or several times a frame, always
with the same `step.dt`. Forces, custom movement, abilities and anything that
has to come out the same at any frame rate, or replay exactly, belong here.
Timers fire in these steps too.

Inside it `delta_time()` reads `step.dt` and `elapsed_time()` the step's time.
`step.input.pressed(action)` is true on exactly one step after a press, even
when the frame the press arrived on ran no step or several. Asking about an
action that is unset or failed to load stops the game (see [`Asset`](Asset.md)).

### Example
```cpp
void fixed_tick(const fjell::FixedStep& step) override {
    if (step.input.held(thrust_action)) {
        fjell::physics::apply_force(id(), {0.0f, thrust, 0.0f});
    }
}
```

---

## ScriptComponent::late_tick

### Syntax
```cpp
virtual void late_tick(float dt);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `dt` | `float` | As `tick` receives it. |

### Remarks
Called every frame after the simulation has stepped, when every node sits
where it will be drawn. Cameras and anything that follows what is on screen
belong here. Read the followed node through
[`Node3D::render_transform()`](Node3D.md#node3drender_transform), not
`position()`: `position()` is where the last step left it, up to a step ahead
of where it is drawn, and a follower reading it judders.

### Example
```cpp
void late_tick(float /*dt*/) override {
    const fjell::Transform drawn = fjell::Node3D{target_id_}.render_transform();
    transform().position = drawn.position + offset_;
}
```

---

## ScriptComponent::end_play

### Syntax
```cpp
virtual void end_play();
```

### Remarks
Called once before the component is destroyed — by scene unload, node destruction, or Build & Reload. The component is still fully valid; engine APIs work normally.

Use this to:
- Cancel timers ([`Timer::cancel`](Timer.md#timercancel))
- Destroy viewports created by this component ([`viewport::destroy`](viewport_api.md#viewportdestroy))
- Disconnect delegate `Connection`s if they outlive `this` (not needed for member-owned connections — their destructors handle it)

---

## ScriptComponent::on_collision_enter

### Syntax
```cpp
virtual void on_collision_enter(uint32_t other_id,
                                const glm::vec3& point,
                                const glm::vec3& normal);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `other_id` | `uint32_t` | Node ID of the other collider. |
| `point` | `glm::vec3` | World-space contact point. |
| `normal` | `glm::vec3` | Surface normal at the contact, pointing **away** from the other body. |

### Remarks
Fires once per contact pair when the contact begins. Requires both nodes to have collider components and at least one to have a rigidbody. Not fired for triggers — see `on_trigger_enter`.

---

## ScriptComponent::on_collision_exit

### Syntax
```cpp
virtual void on_collision_exit(uint32_t other_id);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `other_id` | `uint32_t` | Node ID of the other collider. |

### Remarks
Fires once when a contact pair ends. No contact point/normal — by definition there's no contact anymore.

---

## ScriptComponent::on_trigger_enter

### Syntax
```cpp
virtual void on_trigger_enter(uint32_t other_id);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `other_id` | `uint32_t` | Node ID of the other body. |

### Remarks
Fires when another body enters a `ColliderComponent` flagged `is_trigger = true`. Triggers don't generate physical response — they only fire callbacks.

### Example
```cpp
void on_trigger_enter(uint32_t /*other*/) override {
    fjell::vfx::spawn(collect_effect, transform().position);
    self().destroy();
}
```

---

## ScriptComponent::on_trigger_exit

### Syntax
```cpp
virtual void on_trigger_exit(uint32_t other_id);
```

Fires once when another body leaves a trigger volume.

---

## ScriptComponent::id

### Syntax
```cpp
[[nodiscard]] uint32_t id() const;
```

### Return value
The owning node's ID. Same value as `self().id()`.

### Remarks
Useful as the first argument to APIs that take node IDs ([`animator::set_param`](animator_api.md#animatorset_param), [`scene::spawn`](scene_api.md#scenespawn-empty), etc.).

---

## ScriptComponent::name

### Syntax
```cpp
[[nodiscard]] const char* name() const;
```

### Return value
The owning node's name. Pointer owned by the engine — do not free.

---

## ScriptComponent::self

### Syntax
```cpp
[[nodiscard]] Node3D self() const;
```

### Return value
A [`Node3D`](Node3D.md) handle wrapping this component's owning node ID.

### Example
```cpp
self().destroy();
auto* other = self().get_component<EnemyController>();
```

---

## ScriptComponent::transform

### Syntax
```cpp
[[nodiscard]] Transform&       transform();
[[nodiscard]] const Transform& transform() const;
```

### Return value
Reference to the owning node's transform. Writes take effect immediately and propagate to children on the next transform update.

### Remarks
The reference is valid for the lifetime of the node. Don't dereference after destroying the node.

### Example
```cpp
transform().position += velocity_ * dt;
transform().rotation = glm::angleAxis(angle_, glm::vec3{0, 1, 0});
```

---

## ScriptComponent::input

### Syntax
```cpp
[[nodiscard]] const Input& input() const;
```

### Return value
The current frame's [`Input`](Input.md) state. Reference is valid until the next frame.

### Remarks
For one-off polling. Prefer [action bindings](input_api.md) for game-facing controls — they support remapping, contexts, and gamepad.

---

## ScriptComponent::delta_time

### Syntax
```cpp
[[nodiscard]] float delta_time() const;
```

### Return value
Same value as the `dt` argument to `tick(dt)`; inside `fixed_tick()`, the step's length. Provided for convenience in methods that don't receive it.

---

## ScriptComponent::elapsed_time

### Syntax
```cpp
[[nodiscard]] float elapsed_time() const;
```

### Return value
Scaled gameplay seconds since the play session started; inside `fixed_tick()`, the step's time. Reset when the scene reloads.

---

## ScriptComponent::world

### Syntax
```cpp
[[nodiscard]] World*       world();
[[nodiscard]] const World* world() const;
```

### Return value
Pointer to the [`World`](World.md) struct — input, dt, elapsed_time and the fixed step's time. The individual accessors above are convenience wrappers around this.

## Full example

```cpp
#include "fjell/audio_api.hpp"
#include "fjell/component.hpp"
#include "fjell/log_api.hpp"
#include "fjell/registry.hpp"
#include "fjell/vfx_api.hpp"

class Coin : public fjell::ScriptComponent {
    FJELL_COMPONENT(Coin)

    [[fj::property]] float spin_speed = 90.0f;
    [[fj::property]] int   points     = 10;
    [[fj::property]] fjell::Asset<fjell::Vfx>   collect_effect;
    [[fj::property]] fjell::Asset<fjell::Audio> collect_sound;

    void begin_play() override {
        base_y_ = transform().position.y;
    }

    void tick(float dt) override {
        auto rot = transform().rotation;
        rot = glm::rotate(rot, glm::radians(spin_speed * dt), {0, 1, 0});
        transform().rotation = rot;
    }

    void on_trigger_enter(uint32_t /*other*/) override {
        fjell::vfx::spawn(collect_effect, transform().position);
        fjell::audio::play(collect_sound, transform().position);
        self().destroy();
    }

private:
    float base_y_ = 0.0f;
};
```

## See also
- [`Actor`](Actor.md) — base for non-component objects
- [`UiWidget`](UiWidget.md) — base for UI controllers
- [`Node3D`](Node3D.md) — handle returned by `self()`
- [`World`](World.md) — per-frame state struct
- [Components guide](../Components.md) — authoring guide
