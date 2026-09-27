# Scripting Overview

Fjell scripts are plain C++23 that live in your game project, compile into a shared library (`game.so` on Linux), and get loaded by the engine at runtime. The engine reloads the library on demand — this is "Build & Reload" in the editor — and your scene keeps running across the reload.

This page is the mental model. For the call-by-call catalog, see [Engine API](Engine-API.md).

## The two base classes

There are two things you'll subclass.

### `ScriptComponent` (alias: `Component`)
Attached to a `SceneNode`. Has a transform, runs lifecycle hooks, can access engine state via `world()` and `input()`. **This is the one you'll use 95% of the time.**

```cpp
#include "fjell/component.hpp"
#include "fjell/log_api.hpp"
#include "fjell/registry.hpp"

class Greeter : public fjell::ScriptComponent {
    FJELL_COMPONENT(Greeter)

    [[fj::property]] std::string message = "hello";

    void begin_play() override { fjell::Log::info(message.c_str()); }
    void tick(float dt) override { (void)dt; }
};
```

### `UiWidget` (alias: `UiComponent`)
The script-side counterpart to a `.fjwidget` template, its controller: lifecycle hooks, `[[fj::action]]` methods the markup calls, and fields and Models the markup reads where they are. Bound to a UI tree instead of a scene node. See [Widgets](../Game-UI/Widgets.md) and [Models](../Game-UI/Models.md).

There's also an `Actor` base class in `fjell/actor.hpp`, but in practice you'll write `ScriptComponent`s attached to nodes. Actor exists for non-component game objects with their own transform.

## Lifecycle

| Hook | When |
|---|---|
| `begin_play()` | Once, after the scene is loaded and the component is wired in, or at once when it is added to a running game. Inputs are ready, refs are resolved. |
| `tick(float dt)` | Every frame while playing, before the simulation steps. `dt` is scaled gameplay time (see `Time::set_scale`); 0 while paused. |
| `fixed_tick(const FixedStep& step)` | Every fixed step of the simulation, before physics moves anything: none, one or several times a frame, always `step.dt` long. |
| `late_tick(float dt)` | Every frame, after the simulation has stepped and everything sits where it will be drawn. |
| `end_play()` | Once, when play stops, the node is destroyed or scripts reload. Its input bindings and timers go with it. |

`UiWidget`s get `begin_play()`, `tick()` and `end_play()`.

Plus collision callbacks on `ScriptComponent`:

```cpp
void on_collision_enter(uint32_t other_id, const glm::vec3& point, const glm::vec3& normal);
void on_collision_exit(uint32_t other_id);
void on_trigger_enter(uint32_t other_id);
void on_trigger_exit(uint32_t other_id);
```

`other_id` is the other node's id — pass it to `scene::find_node` or look up a component on the node.

## The three ticks

The simulation — physics, characters, timers — advances in fixed steps, 60 a
second unless the project sets another rate, whatever the frame rate is.
Frames and steps don't line up: at 144 fps most frames run no step, at 30 each
runs two. Each hook has its job:

| Hook | Runs | Put here | Don't |
|---|---|---|---|
| `tick(dt)` | Once a frame, before the steps | Reacting to input, game logic, deciding what things should do: set a character's `move_intent`, start an ability, change state | Integrate motion the simulation owns with `dt` |
| `fixed_tick(step)` | Once per step | Custom movement, forces, anything that has to come out the same at any frame rate or replay exactly | Follow things on screen |
| `late_tick(dt)` | Once a frame, after the steps | Cameras, and anything that follows what is drawn | Move things the simulation owns |

Most scripts need only `tick()`, input handlers and a character's intent.
Custom physics wants `fixed_tick()`; followers want `late_tick()`.

```cpp
class Rocket : public fjell::ScriptComponent {
    FJELL_COMPONENT(Rocket)

    [[fj::property]] float thrust = 40.0f;
    [[fj::property]] fjell::Asset<fjell::InputAction> boost;

    void fixed_tick(const fjell::FixedStep& step) override {
        // A force acts on the step it is applied in: apply it in every step.
        fjell::physics::apply_force(id(), {0.0f, thrust, 0.0f});
        if (step.input.pressed(boost)) {
            fjell::physics::apply_impulse(id(), {0.0f, 8.0f, 0.0f});
        }
    }
};

class ChaseCamera : public fjell::ScriptComponent {
    FJELL_COMPONENT(ChaseCamera)

    [[fj::property]] std::string target = "Rocket";

    void begin_play() override { target_id_ = fjell::scene::find_node(target.c_str()); }

    void late_tick(float /*dt*/) override {
        if (target_id_ == 0) return;
        // Where the rocket is drawn this frame, not where the last step left it.
        transform().position = fjell::Node3D{target_id_}.render_transform().position + offset_;
    }

private:
    uint32_t target_id_{0};
    glm::vec3 offset_{0.0f, 2.0f, -6.0f};
};
```

**Where a node is, and where it is drawn.** `position()` and `transform()` are
the simulation's state: where the last step left a node. A node the steps
moved is drawn blended between the last two steps, so it glides at any frame
rate; `Node3D::render_transform()` is that drawn placement, in world space. A
camera or marker that follows `position()` steps while its target glides —
follow `render_transform()` from `late_tick()`.

**Moving without a blend.** `Node3D::teleport(position, rotation)` puts a node
somewhere at once: it is drawn there rather than slid there, and a body or
character on it moves along, keeping its velocity. Use it for respawns and
doors to elsewhere. Writing `position()` on a dynamic body does nothing
lasting — physics puts it back at the next step.

**Input in a step.** `step.input` is the input that step sees:
`pressed(action)`, `released(action)`, `held(action)`, `axis1d(action)`,
`axis2d(action)`. A press is seen by exactly one step, the first to begin
after it, even when its frame ran no step or several. Handlers bound with
`input::bind` still fire once a frame, before `tick()`.

**Characters.** A `CharacterComponent` moved by intent needs no `fixed_tick()`:
set `move_intent` from an input handler or `tick()` and call `jump()`, and the
engine steers it every step. What the frame sets steers every step that frame
runs; set in `fixed_tick()`, it steers that step only. See
[Physics](../Physics.md#moving-it).

**Time inside a step.** `delta_time()`, `world()->delta_time` and
`world()->elapsed_time` read the step's own length and time inside
`fixed_tick()`, so code that reads them there is right. `Time::fixed_delta()`
and `world()->fixed_delta_time`, `fixed_time`, `fixed_step_index` read the
step from anywhere. Timers fire inside the steps.

**Which script runs first.** Scripts run type by type: every script of one
type, then every script of the next. A type says where it goes with
`[[fj::tick(order = n)]]` on its class: lower runs earlier, and a type that
says nothing is 0. Types of the same order run by name, and the scripts of one
type in the order they joined the scene. Where one script's `tick()` reads what
another's wrote this frame, give the writer the lower order rather than relying
on where the nodes sit, which does not decide it. What follows the picture
belongs in `late_tick()`, which runs after every `tick()` and every step.

```cpp
class [[fj::tick(order = -10)]] InputRouter : public fjell::ScriptComponent {
    FJELL_COMPONENT(InputRouter)
    void tick(float dt) override;   // runs before order 0
};
```

**How often a script ticks.** `[[fj::tick(interval = s)]]` calls a type's
`tick()` about every `s` seconds instead of every frame, and hands it the game
time since its last tick, so `position += velocity * dt` still covers the
distance. The scripts of one type are spread across the interval rather than
ticking on the same frame. `fixed_tick()` still runs every step and
`late_tick()` every frame; an interval is for work that does not need either.
Both keys go in one attribute: `[[fj::tick(order = 5, interval = 0.5)]]`.

A script can stop ticking altogether with `set_tick_enabled(false)`: no
`tick()`, `fixed_tick()` or `late_tick()` until it turns ticking back on.
Everything that arrives as an event still reaches it — delegates, input
actions, timers, collision and trigger callbacks, weather, time and peer
events, RPCs and UI actions — so a door can sleep until something enters it and
tick only while it opens. Each of the three is called only on types that
override it, so a type that overrides none costs nothing per frame.

```cpp
class [[fj::tick(interval = 0.5)]] Scanner : public fjell::ScriptComponent {
    FJELL_COMPONENT(Scanner)
    void tick(float dt) override;   // about twice a second, dt ≈ 0.5
};

void Door::begin_play() { set_tick_enabled(false); }
void Door::on_trigger_enter(uint32_t) { set_tick_enabled(true); }
```

**What the editor tells you.** A script whose `tick()` or `late_tick()` moves
a dynamic body, or writes a character's velocity against its movement mode,
is reported once in the console with what to do instead, and fails a
functional test. `fjell <project> --run-tests --pacing 30,144,jitter` plays the
game's tests at several frame rates and reports the first step where they
differ (see [Functional Tests](Functional-Tests.md#pacing)).

## Reflection — `[[fj::property]]`, `[[fj::state]]`, `[[fj::model]]`, `[[fj::prop]]`, `[[fj::action]]`

Fields you annotate are picked up by the `fjreflect` codegen (run by Build & Reload) and surfaced in the editor's properties panel, serialized to `.fjscene`, and traced by the GC.

```cpp
class Coin : public fjell::ScriptComponent {
    FJELL_COMPONENT(Coin)

    [[fj::property]] float spin_speed = 90.0f;
    [[fj::property]] int   points     = 10;
    [[fj::property]] glm::vec3 axis{0, 1, 0};
    [[fj::property]] fjell::Asset<fjell::Vfx>   collect_effect;
    [[fj::property]] fjell::Asset<fjell::Audio> collect_sound;
};
```

What gets reflected depends on context:
- **`[[fj::property]]`** — on `ScriptComponent` fields. Shows up in the scene properties panel.
- **`[[fj::action]]`** — on `UiWidget` methods taking nothing, an `int`, a `float` or a `glm::vec2`, private ones too. Markup calls them by name, `on:click="select_slot({slot.key})"`, with the argument converted to the parameter's type.
- **`[[fj::state]]`** — on fields and `const` getters of any component: runtime values the game UI can read. Shown in the properties panel but not editable, and never saved. A state field may hold a value, a struct or a `std::vector` of either; a getter returns a value and runs each time it is read, so keep it cheap.
- **`[[fj::model]]`** — on `UiWidget` fields holding a struct: a Model the widget's markup reads by the field's name, `{hud.selected}` for `[[fj::model]] HudModel hud`. The struct is reflected in full, every public member with no attribute needed (see [Widgets](../Game-UI/Widgets.md#models)).
- **`[[fj::prop]]`** — on `UiWidget` fields holding a `bool`, `int`, `float` or `std::string`: filled from the widget's prop of the same name before `begin_play()` and every `tick()` (see [Models](../Game-UI/Models.md#the-controllers-side)).

An authored value is a property and a value the game changes while playing is state. Either may be private:

```cpp
class PlayerStats : public fjell::ScriptComponent {
    FJELL_COMPONENT(PlayerStats)
public:
    [[fj::state]] bool low_health() const { return hp * 4 < max_hp; }
private:
    [[fj::property]] int max_hp = 100; // authored, saved with the scene
    [[fj::state]] int hp = 100;        // runtime: shown, never saved
};
```

Supported field types (from `FjellFieldType` in `fjell/api.h`):

| Type | C++ | Inspector |
|---|---|---|
| `FLOAT` | `float` | drag float (range, speed) |
| `INT` | `int` / `int32_t` | drag int |
| `BOOL` | `bool` | checkbox |
| `VEC2/3/4` | `glm::vec2` etc | drag floats |
| `COLOR3/4` | `glm::vec3` / `vec4` | color picker |
| `STRING` | `std::string` | text field |
| `ENUM` | `enum class` | dropdown |
| `REF` | `fjell::Asset<T>` | asset picker (Vfx, Audio, InputAction, ...) |
| `WEAK_REF` | `fjell::WeakRef<T>` | scene-object picker |

### When fjreflect runs
Build & Reload runs fjreflect over every header in `scripts/` before it builds the
module. It writes one `scripts/generated/<header>.generated.cpp` per header, holding
the field tables of every component in it, and `scripts/register_all.cpp`, the
module's entry points that hand those tables to the engine. A file is rewritten
only when its content changes, so an unchanged header costs no recompile, and the
outputs of a header you delete or rename are removed. Offsets come from the
compiler that builds the module (`offsetof` in the generated code), so a
reflected field may be private.

A plain `cmake --build` in `scripts/` does not run fjreflect; use Build & Reload
after changing a header. Anything fjreflect cannot reflect (an unknown `fj::`
attribute, an attribute on a function or a static, a type it has no field kind
for) stops the build with the file and line.

## Reference types

Three flavors. Pick by lifetime:

### `fjell::Asset<T>` — asset reference
For *engine-owned assets*: `Vfx`, `Audio`, `InputAction`, `Preset`, etc. The handle is 64 bits, stable, and resolved through the engine on every dereference (cheap — a pool lookup).

```cpp
[[fj::property]] fjell::Asset<fjell::Audio> hit_sound;

void play_it() {
    fjell::audio::play(hit_sound, transform().position);
}
```

Assigned in the inspector via drag-drop or asset picker. `hit_sound.valid()` tells you whether it points at anything.

### `fjell::WeakRef<T>` — non-owning scene reference
For *scene-side things that might be destroyed under you*: another node, another component. Uses (id, generation) so a destroyed-and-replaced node doesn't resolve to the wrong object.

```cpp
[[fj::property]] fjell::WeakRef<EnemyController> target;

void shoot() {
    if (auto* t = target.get()) {
        t->take_damage(10);
    }
}
```

Always null-check `get()` — that's the whole point of WeakRef.

### Raw pointers
Avoid them across the hot-reload boundary. Within one tick, a raw `T*` from `Node3D::get_component<T>()` is fine; storing one in a member is asking for a use-after-reload. Use `WeakRef` for persistent references.

## Talking to the scene

Inside a `ScriptComponent`:

| Call | Use |
|---|---|
| `transform()` | Read/write position, rotation, scale of the node you're attached to |
| `self()` | The `Node3D` handle for your own node |
| `id()` | `uint32_t` node id (e.g. for `animator::set_param`) |
| `input()` | Const ref to the current `Input` state |
| `delta_time()`, `elapsed_time()` | Scaled gameplay clock; the step's own inside `fixed_tick()` |

Physics queries and forces go through `fjell::physics` (`fjell/physics_api.hpp`).

To find or spawn nodes anywhere in the scene, use `fjell::scene` (see [Engine API](Engine-API.md)).

## Events with `Delegate<>`

`fjell::Delegate<void(Args...)>` is a multicast event. Listeners get an RAII `Connection` back — drop it to unsubscribe.

```cpp
class PlayerStats : public fjell::ScriptComponent {
    FJELL_COMPONENT(PlayerStats)

    fjell::Delegate<void(int, int)> on_health_changed;
    fjell::Delegate<void()>         on_death;

    void take_damage(int amount) {
        int old = hp;
        hp = std::max(0, hp - amount);
        on_health_changed.broadcast(old, hp);
        if (hp <= 0) on_death.broadcast();
    }
};

// Listener side:
fjell::Connection conn = stats->on_death.bind([this] { play_death_anim(); });
```

Connections must be kept alive — store them as members. Calling `disconnect()` (or letting the `Connection` drop) removes the listener safely, even mid-broadcast.

## Hot-reload model

When you click Build & Reload in the editor:
1. The engine calls `end_play()` on every component, then unloads `game.so`.
2. CMake builds your project. (Errors surface in the console panel.)
3. Engine loads the new `.so`, registers types, recreates components from `.fjscene`, calls `begin_play()` again.

What survives:
- Anything in `.fjscene` (the serialized field values).
- Engine-side state (assets, viewports, scene structure).

What does **not** survive:
- Any runtime member that isn't a `[[fj::property]]`. Reset it in `begin_play()`.
- Raw pointers between components (re-resolve via `WeakRef` or `get_component<T>()`).
- Bound input actions, timers, delegate connections — `begin_play()` should re-bind everything.

If a reload crashes you're usually looking at one of:
- Changed a `virtual` signature without bumping `FJELL_API_VERSION` or running a clean reconfigure.
- Held a raw pointer to something the engine recreated.
- Stored a `Connection` in a member that didn't get cleared in `end_play()`.

## Garbage collection

Field-based GC traces reachable `Ref<>` / `WeakRef<>` graphs starting from registered components and widgets. The codegen emits a `trace` function per type — that's the function pointer in `FjellComponentTypeOOP::trace`. You don't need to call into it; just annotate fields with `[[fj::property]]` and the codegen does the rest.

## SDK headers — where to look

All under `src/scripting/fjell/`. The shape of the SDK:

| Header | What you'll find |
|---|---|
| `api.h` | C ABI: field types, action types, key codes (`FJELL_KEY_*`, `FJELL_MOUSE_*`), `EInputTrigger` |
| `component.hpp` | `ScriptComponent`, lifecycle, collision hooks |
| `fixed_step.hpp` | `FixedStep`, `StepInput` — what `fixed_tick()` receives |
| `actor.hpp` | `Actor` base for non-component objects |
| `node.hpp` | `Node3D` handle, `get_component<T>` |
| `transform.hpp` | `Transform` struct, euler helpers, `decompose_matrix` |
| `world.hpp` | `World` — frame state (input, delta_time, the fixed step's time) |
| `input.hpp` | `Input` class — `is_key_pressed`, mouse position, cursor capture |
| `ref.hpp`, `weak_ref.hpp` | `Asset<T>`, `WeakRef<T>` |
| `delegate.hpp` | `Delegate<>`, `Connection` |
| `registry.hpp` | `FJELL_COMPONENT`, `FJELL_NOTIFY`, `FJELL_REGISTER` |
| `ui_widget.hpp`, `ui_registry.hpp` | `UiWidget`, `FJELL_UI_WIDGET` |
| `notify.hpp` | `AnimNotify`, `AnimNotifyState` |
| `engine_components.hpp` | Engine-side component structs: `MeshComponent`, `SpriteComponent`, `RigidbodyComponent`, `ColliderComponent`, `CharacterComponent` |
| `*_api.hpp` | Static APIs — see [Engine API](Engine-API.md) |
