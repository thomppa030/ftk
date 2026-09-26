# fjell::UiWidget

Base class for game UI widgets — the C++ controller side of a `.fjwidget` template. Aliased as `fjell::UiComponent`.

| | |
|---|---|
| Header | `fjell/ui_widget.hpp` |
| Source | (header-only base; binding lives in `UiSubsystem`) |

## Syntax

```cpp
namespace fjell {

class UiWidget {
public:
    virtual ~UiWidget() = default;

    virtual void begin_play();
    virtual void tick(float dt);
    virtual void end_play();

    [[nodiscard]] float          delta_time()   const;
    [[nodiscard]] float          elapsed_time() const;
    [[nodiscard]] const Input&   input()        const;
    [[nodiscard]] World*         world();
    [[nodiscard]] const World*   world() const;

    [[nodiscard]] UiWidgetContext& ui();
};

class UiWidgetContext {
public:
    void        set(const std::string& key, const std::string& value);
    std::string get(const std::string& key) const;
};

using UiComponent = UiWidget;

}
```

## Remarks

A `UiWidget` is bound to a `.fjwidget` template at runtime by the `UiSubsystem`. The widget's `[[fj::bind]]` fields are accessed by `{key}` interpolation in the FJUI markup, and its `[[fj::action]]` methods are callable from `on:click="name"` attributes.

Use `FJELL_UI_WIDGET(MyWidget)` to register the subclass. The fjreflect codegen also picks up `[[fj::bind]]` fields and `[[fj::action]]` methods for the editor's UI inspector.

Like [`ScriptComponent`](ScriptComponent.md), widgets have a `begin_play` / `tick` / `end_play` lifecycle. They differ in that they:
- Have no transform — they live in the UI tree, not the scene graph.
- Have no collision hooks.
- Access state through a [`UiWidgetContext`](#uiwidgetcontext) (via `ui()`) rather than `transform()`.

See [Widgets](../../Game-UI/Widgets.md) for the authoring side.

## Lifecycle functions

| Name | Description |
|---|---|
| `begin_play` | One-time init. Bind actions, set initial state. |
| `tick(float dt)` | Per-frame update. Most widgets don't need this. |
| `end_play` | One-time teardown. |

## Accessors

| Name | Description |
|---|---|
| `delta_time` | Scaled gameplay dt this frame |
| `elapsed_time` | Scaled time since session start |
| `input` | Frame's [`Input`](Input.md) state |
| `world` | [`World`](World.md) struct |
| `ui` | The widget's [`UiWidgetContext`](#uiwidgetcontext) — bind actions, set/get keys |

## UiWidgetContext

The bridge between the widget controller and the UI tree.

### Functions

| Name | Description |
|---|---|
| [`set`](#uiwidgetcontextset) | Push a value into the UI context (drives `{key}` interpolation) |
| [`get`](#uiwidgetcontextget) | Read back a value |

Actions are not bound here: markup calls the widget's `[[fj::action]]` methods (see
[Actions](#actions)).

---

### UiWidgetContext::set

```cpp
void set(const std::string& key, const std::string& value);
```

Pushes `value` into the context under `key`. Any `{key}` template references in the bound `.fjui` markup re-resolve on the next layout pass.

Values are stringly-typed. For numbers, format yourself:

```cpp
ui().set("hp", std::to_string(player_hp));
```

---

### UiWidgetContext::get

```cpp
[[nodiscard]] std::string get(const std::string& key) const;
```

Returns the current value, or `""` if the key isn't set.

---

## Actions

`on:click="name"` and a widget's events call the controller method of that name
marked `[[fj::action]]`. It takes nothing, an `int`, a `float` or a `glm::vec2`, and
the argument written in the markup is converted to it: `on:click="select_slot(3)"`,
or `select_slot({slot.key})` with the value the path reads. The method may be private.
The nearest controller up from the click that declares the action answers it.

## Example

Header:

```cpp
#pragma once
#include "fjell/ui_registry.hpp"

class hud_controller : public fjell::UiWidget {
    FJELL_UI_WIDGET(hud_controller)

public:
    [[fj::action]] void select_slot(int slot); // on:click="select_slot({slot.key})"

private:
    int selected_ = 1;
};
```

Source:

```cpp
#include "hud_controller.hpp"
#include "fjell/log_api.hpp"

void hud_controller::select_slot(int slot) {
    selected_ = slot;
    fjell::Log::info_f("HUD: selected slot %d", slot);
}
```

## See also
- [Widgets](../../Game-UI/Widgets.md) — authoring `.fjwidget` files
- [UiContext](../../Game-UI/UiContext.md) — the broader game ↔ UI bridge
- [`ScriptComponent`](ScriptComponent.md) — scene-side counterpart
