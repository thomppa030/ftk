# UiContext — Game ↔ UI Bridge

The one object game code uses to push state into a UI and to receive its events. It
backs `{key}` interpolation, `if=`, `class:` toggles, `foreach` and `on:click`.
Declared in `ui_context.hpp`; widgets reach it through `UiWidget::ui()`.

## The API

```cpp
ctx.set("player.health", "72");        // values are strings
ctx.set_int("hp", 72);                  // "72"
ctx.set_float("ratio", 0.5f);           // "0.5"
ctx.set_bool("show_debug", true);       // "1"
std::string hp = ctx.get("player.health");   // "" when never set
ctx.dispatch("use_item");               // fire an action from code
ctx.dispatch("select_slot", {"3"});     // with arguments
```

Bindings:

```cpp
Connection c = ctx.bind_action("use_item", [&] { inventory.use_selected(); });
Connection d = ctx.bind_action("select_slot",
    [&](const std::vector<std::string>& args) { select(std::stoi(args[0])); });
```

`bind_action` returns a `Connection` that unbinds when it is destroyed, so a system
keeps its connections as members. Inside a `UiWidget`, `ui().bind_action()` stores
the connection for you and releases it with the widget.

Lists, for `foreach`:

```cpp
std::vector<fjell::UiContext> items;
for (const auto& entry : inventory) {
    items.emplace_back();
    items.back().set("name", entry.name);
    items.back().set("count", entry.count);
}
ctx.set_list("inventory", std::move(items));
```

Each item is a full context of its own, parented to the list's owner, and the
markup element carrying `foreach="inventory"` is cloned once per item. `{__index}`
is the item's position.

## Scoping

Contexts form a chain. Every widget instance gets its own context whose parent is
the placing widget's, up to the root UI. `get()` walks up until a key is found, so a
child widget may read the HUD's `{fps}` without it being passed as a prop, and a
prop shadows an inherited key of the same name. `dispatch()` bubbles the same way:
the nearest context with a handler wins.

A widget's props land in its context before `begin_play()`. `[[fj::bind]]` fields
of a `UiWidget` are published into its context every tick, so a controller can
simply assign to a field.

## Change tracking

`set()` with an unchanged value costs a lookup and nothing else, so calling it from
`tick()` is fine. Every real change bumps the context's generation; the UI resolves
templates only when a generation moved, and a resolved template that changed size
triggers a relayout.

## Lifetime

A `UiSubsystem` owns one root context per placed widget and the instance contexts
below it. They are created at play start and torn down at play end, and a widget
hot-reload rebuilds the instance contexts and re-runs `begin_play()`, so anything a
controller sets there comes back. A raw `GameUi` (the UI editor preview, or code
that drives a UI without the subsystem) owns its own root context, reached with
`game_ui.context()`.

## When to use it

Push what the UI reads, and nothing else. A HUD needs the health value, not the
`Player`; convert at the boundary and keep formatting on the game side, since values
are strings.

```cpp
void hud_controller::begin_play() {
    ui().bind_action("select_slot", [this](const std::vector<std::string>& a) {
        selected = std::stoi(a[0]);              // [[fj::bind]] int selected
    });
    fjell::Node3D player(fjell::SceneAPI::find_node_with<PlayerStats>());
    if (auto* stats = player.get_component<PlayerStats>()) {
        health_connection_ = stats->on_health_changed.bind(
            [this](int, int hp) { ui().set_int("player.health", hp); });
    }
}
```
