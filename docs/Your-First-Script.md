# Your First Script

Build the smallest possible Fjell component — a node that spins — from empty file to working in the editor. About five minutes if your project is already set up.

If you don't have a project yet, see [Creating a Project](Creating-a-Project.md) first.

## What we're making

A `Spinner` component you can drop onto any node. It rotates the node around the Y axis at a configurable speed (set per-instance in the editor) and logs a line on play.

```cpp
class Spinner : public fjell::ScriptComponent {
    FJELL_COMPONENT(Spinner)

    [[fj::property]] float speed = 90.0f;   // degrees/sec

    void begin_play() override;
    void tick(float dt) override;
};
```

By the end you'll see:
- The component in the editor's **Add Component** menu.
- A `Speed` field on it in the **Properties** panel.
- The node spinning in the viewport while playing.
- A "Spinner: hello" line in the **Console** panel.

## 1. Create the header

In your project's `scripts/` folder, create `spinner.hpp`:

```cpp
#pragma once
#include "fjell/component.hpp"
#include "fjell/registry.hpp"

class Spinner : public fjell::ScriptComponent {
    FJELL_COMPONENT(Spinner)

    [[fj::property]] float speed = 90.0f;

    void begin_play() override;
    void tick(float dt) override;
};
```

The two things that turn a plain class into a real component:

| Element | Effect |
|---|---|
| `FJELL_COMPONENT(Spinner)` | Registers the type. Without it, the engine doesn't know about `Spinner` and the editor won't list it. |
| `[[fj::property]]` | Tells `fjreflect` to surface `speed` in the inspector and serialize it to `.fjscene`. |

See [`ScriptComponent`](Scripting/API/ScriptComponent.md) for everything you inherit from the base.

## 2. Create the implementation

Next to it, `spinner.cpp`:

```cpp
#include "spinner.hpp"
#include "fjell/log_api.hpp"
#include <glm/gtc/quaternion.hpp>

void Spinner::begin_play() {
    fjell::Log::info("Spinner: hello");
}

void Spinner::tick(float dt) {
    auto rot = transform().rotation;
    rot = glm::rotate(rot, glm::radians(speed * dt), glm::vec3{0, 1, 0});
    transform().rotation = rot;
}
```

`dt` is already scaled by [`Time::scale`](Scripting/API/Time.md) — slow-mo and pause both work for free.

## 3. Build & Reload

In the editor, click **Build & Reload** in the toolbar (or your project's build button).

What happens behind the scenes:
1. The engine calls `end_play()` on every existing component and unloads `game.so`.
2. `fjreflect` reads your headers' `[[fj::property]]` annotations and updates `scripts/generated/*.generated.cpp` and `scripts/register_all.cpp`.
3. CMake rebuilds the project — output goes to `<project>/game.so` (the engine looks there).
4. Engine reloads the `.so`, re-registers types, recreates components from the scene file, calls `begin_play()` on each.

If the build fails, the console panel shows the compiler output. Fix and click Build & Reload again.

> If you change a `virtual` function signature on a `ScriptComponent` subclass and reload crashes, run a full reconfigure (`cmake -B build` in `scripts/`) — vtable layout changed and incremental builds occasionally miss it.

## 4. Attach to a node

1. In the **Hierarchy** panel, select any node (or spawn an empty: right-click → **Add → Empty Node**).
2. In the **Properties** panel, click **Add Component** at the bottom and pick **Spinner**.
3. The `Speed` field appears. Try `45` or `180`.

## 5. Play

Hit **Play** in the toolbar (or the editor's play shortcut). You should see:
- The node spinning around its Y axis.
- `Spinner: hello` in the **Console** panel.

Stop play. Edit `speed` to a different value. Play again — the node spins at the new rate. The value persists because `[[fj::property]]` made it serializable.

## What just happened

A few things to notice that you'll rely on for every future component:

**The transform is yours to write.** `transform()` returns a reference. Writing to `transform().rotation` (or `position`, `scale`) takes effect immediately. See [`Transform`](Scripting/API/Transform.md).

**`dt` is the scaled gameplay clock.** Animations, timers, and your `tick` all use it. If the game is paused, `tick` is not called at all.

**The inspector reads your annotations.** `[[fj::property]] float speed = 90.0f;` becomes a labeled slider with `90` as the default. Adding richer hints — `[[fj::property, fj::range(0, 360), fj::tooltip("degrees per second")]]` — refines the UI without code changes. See [`Components`](Scripting/Components.md) for the full annotation set.

**Component instances are per-node.** Drop the same component onto five nodes and each gets its own `speed`. State is per-instance.

## Next steps

Add behavior incrementally. Each of these is a one-line change to `spinner.cpp` plus optionally a new `[[fj::property]]` field:

### Spin around a configurable axis

```cpp
[[fj::property]] glm::vec3 axis{0, 1, 0};

// in tick:
rot = glm::rotate(rot, glm::radians(speed * dt), glm::normalize(axis));
```

### Pause spinning on a key

```cpp
void tick(float dt) override {
    if (input().is_key_held(FJELL_KEY_SPACE)) return;
    /* ... */
}
```

(Needs `#include "fjell/api.h"` for the key constant. See [`Input`](Scripting/API/Input.md).)

### Spawn a particle effect when spinning passes a full rotation

```cpp
[[fj::property]] fjell::Ref<fjell::VfxAsset> burst;

void tick(float dt) override {
    angle_ += speed * dt;
    if (angle_ >= 360.0f) {
        angle_ -= 360.0f;
        fjell::Vfx::spawn(burst, transform().position);
    }
    /* ... */
}

private:
    float angle_{0};
```

Add a `.fjvfx` asset, drag it into the new `Burst` slot in the inspector, hit play. See [`Vfx`](Scripting/API/Vfx.md) and [`Ref`](Scripting/API/Ref.md).

### React to collisions

Add a `ColliderComponent` (set `is_trigger = true`) to the same node, then:

```cpp
void on_trigger_enter(uint32_t /*other*/) override {
    fjell::Log::info("Spinner: something touched me");
}
```

See [`ScriptComponent`](Scripting/API/ScriptComponent.md#collision-functions) for the full collision hook set.

## Where to go from here

- [Scripting Overview](Scripting/Overview.md) — the bigger mental model: lifecycle, hot-reload, references, events.
- [Engine API Index](Scripting/Engine-API.md) — the catalog of every API and base class.
- [Components](Scripting/Components.md) — full reflection annotation set, custom field UI, serialization details.
- [Input Actions](Scripting/Input-Actions.md) — replacing `is_key_held` with rebindable action bindings.
