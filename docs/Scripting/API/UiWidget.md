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

    [[nodiscard]] UiWidgetModels& models();
    template <class T>
    [[nodiscard]] const T* model(const std::string& name) const;
};

class UiWidgetModels {
public:
    void bind(const std::string& name, const Ref<Node3D>& node);
    void clear(const std::string& name);
};

using UiComponent = UiWidget;

}
```

## Remarks

A `UiWidget` is the controller a `.fjwidget` names in its `class`, created by the
`UiSubsystem` for each placement and each widget it places. Its markup reads it where
it is, by name: its `[[fj::model]]` structs and its `[[fj::state]]` fields and
getters. Its `[[fj::prop]]` fields are filled from the widget's props, and its
`[[fj::action]]` methods answer `on:click`. Nothing is published by hand: a
controller changes what the UI shows by assigning to its own fields, and the UI
notices when it next compares what it shows. See [Models](../../Game-UI/Models.md).

Use `FJELL_UI_WIDGET(MyWidget)` to register the subclass; fjreflect picks up the
annotated fields and methods on Build & Reload.

Like [`ScriptComponent`](ScriptComponent.md), widgets have a `begin_play` / `tick` / `end_play` lifecycle. They differ in that they:
- Have no transform — they live in the UI tree, not the scene graph.
- Have no collision hooks.
- Read the game through Models rather than holding pointers into the scene.

See [Widgets](../../Game-UI/Widgets.md) for the authoring side.

## Lifecycle functions

| Name | Description |
|---|---|
| `begin_play` | One-time init, the widget's props already bound. Set up its Models' state. |
| `tick(float dt)` | Per-frame update. Most widgets don't need this. |
| `end_play` | One-time teardown. |

## Accessors

| Name | Description |
|---|---|
| `delta_time` | Scaled gameplay dt this frame |
| `elapsed_time` | Scaled time since session start |
| `input` | Frame's [`Input`](Input.md) state |
| `world` | [`World`](World.md) struct |
| `models` | The widget's [Models](#models): which node each reads |
| `model<T>` | The component a Model shows now, read-only; null while it finds nothing |

## Fields the markup reads

| Annotation | On | What it does |
|---|---|---|
| `[[fj::model]]` | a struct field | A Model: `{hud.selected}` for `[[fj::model]] HudModel hud`. Every public member of the struct is readable, a `std::vector` of structs is a list for `foreach`. |
| `[[fj::state]]` | a field or a `const` getter | Read by name, `if="open"`; a getter runs each time it is read. |
| `[[fj::prop]]` | a `bool`, `int`, `float` or `std::string` field | Filled from the widget's prop of the same name before `begin_play()` and every `tick()`. |
| `[[fj::action]]` | a method | Called from `on:click`, see [Actions](#actions). |

## Models

A `<model>` in the widget's markup reads a component from a node by its `from` rule.
A controller can pick the node at runtime:

```cpp
void target_frame::tick(float) {
    if (target_changed_) models().bind("target", target_id_); // reads that node
}
void target_frame::end_target() { models().clear("target"); }   // back to the rule
```

`model<EnemyStats>("target")` hands the controller the component the markup shows,
or null. A Model name the widget does not declare, or of another type, is reported
once.

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

#include <string>
#include <vector>

struct HotbarSlot {
    int key = 0;
    std::string name;
    bool active = false;
};

struct HudModel {
    std::vector<HotbarSlot> slots;
    int selected = 1;
};

class hud_controller : public fjell::UiWidget {
    FJELL_UI_WIDGET(hud_controller)

public:
    [[fj::model]] HudModel hud;                // foreach="slot in hud.slots"
    [[fj::action]] void select_slot(int slot); // on:click="select_slot({slot.key})"
};
```

Source:

```cpp
#include "hud_controller.hpp"

void hud_controller::select_slot(int slot) {
    hud.selected = slot;
    for (auto& s : hud.slots) s.active = s.key == slot;
}
```

## See also
- [Widgets](../../Game-UI/Widgets.md) — authoring `.fjwidget` files
- [Models](../../Game-UI/Models.md) — what a widget reads, and how
- [`ScriptComponent`](ScriptComponent.md) — scene-side counterpart
