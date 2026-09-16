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
    void        set       (const std::string& key, const std::string& value);
    std::string get       (const std::string& key) const;
    void        bind_action(const std::string& name, std::function<void()> cb);
    void        bind_action(const std::string& name,
                            std::function<void(const std::vector<std::string>&)> cb);
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
| [`bind_action(name, void())`](#uiwidgetcontextbind_action-no-args) | Register a parameter-less action handler |
| [`bind_action(name, void(args))`](#uiwidgetcontextbind_action-args) | Register an action handler that receives parameters from the markup |

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

### UiWidgetContext::bind_action (no args)

```cpp
void bind_action(const std::string& name, std::function<void()> callback);
```

Registers `callback` to fire when an `on:click="name"` attribute (or other dispatch) hits in the bound markup.

```cpp
void begin_play() override {
    ui().bind_action("open_menu", [this] { is_open_ = true; });
}
```

---

### UiWidgetContext::bind_action (args)

```cpp
void bind_action(const std::string& name,
                 std::function<void(const std::vector<std::string>&)> callback);
```

Same as above but receives positional string arguments. The `.fjui` side passes them as a comma-separated list inside the action call: `on:click="select_slot(3)"` → callback receives `{"3"}`.

```cpp
void begin_play() override {
    ui().bind_action("select_slot",
        [this](const std::vector<std::string>& args) {
            if (!args.empty()) selected = args[0];
        });
}
```

The engine stores `Connection`s engine-side, so action bindings clean up automatically when the widget is destroyed — you do not need to track them in members.

## Example

Header:

```cpp
#pragma once
#include "fjell/ui_registry.hpp"

class hud_controller : public fjell::UiWidget {
    FJELL_UI_WIDGET(hud_controller)

public:
    [[fj::bind]] std::string selected;

    void begin_play() override;
};
```

Source:

```cpp
#include "hud_controller.hpp"
#include "fjell/log_api.hpp"

void hud_controller::begin_play() {
    ui().bind_action("select_slot",
        [this](const std::vector<std::string>& args) {
            if (!args.empty()) {
                selected = args[0];
                fjell::Log::info_f("HUD: selected slot %s", selected.c_str());
            }
        });
}
```

## See also
- [Widgets](../../Game-UI/Widgets.md) — authoring `.fjwidget` files
- [UiContext](../../Game-UI/UiContext.md) — the broader game ↔ UI bridge
- [`ScriptComponent`](ScriptComponent.md) — scene-side counterpart
