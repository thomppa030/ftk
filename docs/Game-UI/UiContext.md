# UiContext — Game ↔ UI Bridge

The one object game code uses to push state into a UI and to receive its events. It
backs `{key}` interpolation, `if=`, `class:` toggles, `foreach` and `on:click`.
Declared in `ui_context.hpp`; widgets reach it through `UiWidget::ui()`.

## The API

```cpp
ctx.set("player.health", "72");         // text
ctx.set("hp", UiValue(72));             // an int: {hp} shows "72", a bar reads 72
ctx.set("ratio", UiValue(0.5f));        // a float: shows "0.5"
ctx.set("show_debug", UiValue(true));   // a bool: shows "1", true for if=
std::string hp = ctx.get("player.health");   // as markup shows it; "" when never set
const UiValue* ratio = ctx.find("ratio");    // the value itself; null when never set
ctx.dispatch("use_item");               // raise an action from code
```

Each name holds one `UiValue` (`ui_value.hpp`), which keeps the type it was given:
text, a bool, an int or a float. Markup reads it as text for `{key}`, as a number
for a bar's `value` and `max`, and as true or false for `if=` and `class:`, where
false is no value, `false`, zero, and the texts `""`, `"0"` and `"false"`. Setting
the same type and value again changes nothing. A widget's props are stored as the
types its `<prop>` declarations name, whether they come from a default or from the
placement: `value="{hp}"` on a `float` prop hands over the number itself.

Actions: a widget's controller answers the `[[fj::action]]` methods its type declares,
and code driving a UI without controllers gives a context a handler of its own:

```cpp
ctx.set_action_handler([&](std::string_view action, std::span<const UiValue> args) {
    if (action != "use_item") return false; // not this one: the parent's handler is asked
    inventory.use_selected();
    return true;
});
```

Lists are not published through the context: a `foreach` walks a `std::vector` in a
Model, `foreach="effect, i in hud.effects"` (see [FJUI](FJUI.md)).

## Scoping

Contexts form a chain. Every widget instance gets its own context whose parent is
the placing widget's, up to the root UI. `get()` walks up until a key is found, so a
child widget may read the HUD's `{fps}` without it being passed as a prop, and a
prop shadows an inherited key of the same name. `dispatch()` bubbles the same way:
the nearest context whose handler takes the action answers it.

A widget's props land in its context before `begin_play()`. The fields of a
`UiWidget` are read from the controller itself, after the context's own values, so a
controller can simply assign to a field.

## Change tracking

Nothing has to announce a change. When a tree loads, every binding in it (text,
`if=`, `class:`, `style-*`, `src`, `material`, a bar, a prop, a `foreach`) is parsed
once into a table. Each frame the UI evaluates the bindings on show and compares
what they read with what they read last frame; only a binding whose values changed
is applied, and a change that moves classes, visibility or styles triggers a
relayout. A subtree hidden by `if=` or `display: none` evaluates only its root's
props, `if=` and `class:`, so a closed menu costs almost nothing. `set()` from
`tick()` is fine: it replaces a value, and the next frame's comparison finds out
whether anything shows it.

## Lifetime

A `UiSubsystem` owns one root context per placed widget and the instance contexts
below it. They are created at play start and torn down at play end, and a widget
hot-reload rebuilds the instance contexts and re-runs `begin_play()`, so anything a
controller sets there comes back. A raw `GameUi` (the UI editor preview, or code
that drives a UI without the subsystem) owns its own root context, reached with
`game_ui.context()`.

## When to use it

Push what the UI reads, and nothing else. A HUD needs the health value, not the
`Player`; convert at the boundary and keep formatting on the game side: a script
publishes text through `ui().set()`.

```cpp
void hud_controller::begin_play() {
    fjell::Node3D player(fjell::SceneAPI::find_node_with<PlayerStats>());
    if (auto* stats = player.get_component<PlayerStats>()) {
        health_connection_ = stats->on_health_changed.bind(
            [this](int, int hp) { ui().set("player.health", std::to_string(hp)); });
    }
}
```
