# FJUI — UI Markup

HTML-like markup for in-game UI: HUDs, menus, inventories. It is separate from the
ImGui editor chrome. `fjui_parser.cpp` turns a file into a `UiNode` tree, Yoga lays
it out as flexbox, `ui_renderer.cpp` draws it with SDF rounded rectangles and MSDF
text. Styling is in [FJSS](FJSS.md), composition in [Widgets](Widgets.md).

Today the markup lives inside `.fjwidget` files; a bare `.fjui` is the same body
without the `<widget>` wrapper and props, and the parser reads both.

```html
<panel id="hud" class="overlay">
    <vitals-panel/>
    <div class="debug-info" if="show_debug">
        <text class="fps-display">FPS: {fps}</text>
    </div>
    <div class="bottom-panel">
        <hotbar-slot index="1" key="1" on:activate="select_slot(1)" selected="{selected}">
            <div class="slot-icon" src="assets/textures/test.png"/>
        </hotbar-slot>
    </div>
</panel>
```

## Elements

| Tag | Draws |
|---|---|
| `panel`, `div` | a box; the two are interchangeable, `panel` reads better as a container |
| `text` | its text content, wrapped to its box; the only element that holds text |
| `bar` | a box whose fill is `value` divided by `max`, both templatable; the drawn fill springs after the value rather than jumping |
| `img` | a box, meant to carry a `src` |
| `slot` | inside a widget only: where the caller's children land |
| anything else | a widget from `widgets/<tag>.fjwidget`, dashes allowed |

Every element is a flex item and may be a flex container; there is no inline flow.
Any element takes `src="path"` to draw a texture, resolved against the project, and
`material="name"` to be painted by a `shader_type ui` shader (see
[Materials](Materials.md)); `material` accepts a `{key}` template.

## Attributes

| Attribute | Does |
|---|---|
| `id`, `class` | selector hooks; `class` takes several names separated by spaces |
| `{key}` in text or any attribute | replaced by the context value, every frame it changes; `{key\|pipe}` formats it (see Pipes) |
| `if="key"` | shows the element while `key` is truthy: not empty, not `0`, not `false` |
| `class:name="key==value"` | adds `name` while the comparison holds; either side may be a `{key}` |
| `on:click="action"` | dispatches `action`; `select_slot(3)` passes arguments |
| `foreach="item, i in path"` | repeats the element once per entry of a list, each repeat with its entry as `item` and its position as `i` (`item in path` without one) |
| `style-<property>="value"` | an inline style, templates resolved: `style---fill="{color}"` |
| `value`, `max` on `bar` | fill fraction, also `FILL` inside a ui shader |
| `settle`, `bounce` on `bar` | how the drawn fill follows the value: seconds to come to rest (0.35) and overshoot 0..1 (0.25) |
| `src`, `material` | texture to draw, ui material to paint with |

Comments are `<!-- -->`, elements self-close with `/>`, and attribute values are
double-quoted. Whitespace-only text between elements is dropped.

A `key` may be a path: `{player.hp}` reads the field `hp` of whatever `player` is,
a widget's Model or a struct inside one ([Widgets](Widgets.md#models)). The first
name is found in the nearest scope that has it; each later one is read from the
value before it. A name no scope holds is read whole, dots and all, so values
published as `player.pos` are still found.

## Pipes

A binding may format its value on the way into the markup: `{stamina|round}`,
`{left|seconds}`, `{ratio|percent:1}`. Pipes run left to right, so
`{count|round|times}` rounds, then prefixes. Arguments follow a colon, each literal
text; put one in single quotes to keep its spaces: `{copper|coins:' gold'}`.

| Pipe | Makes | Example |
|---|---|---|
| `round` | the nearest whole number, halves away from zero | `117.6` → `118`, `2.5` → `3` |
| `fixed:n` | `n` decimals (0–9) | `fixed:1`: `0.25` → `0.3` |
| `seconds` | a countdown: whole seconds rounded up, `m:ss` from a minute | `2.4` → `3`, `125` → `2:05` |
| `times` | a count with `×` | `3` → `×3` |
| `percent[:n]` | a 0–1 ratio as a percentage | `0.123` → `12%`, `percent:1` → `12.3%` |

A game adds its own with `[[fj::pipe]]` on a free function in a script header,
returning `std::string` from the value and any literal arguments:

```cpp
[[fj::pipe]] inline std::string coins(int copper, const std::string& unit) {
    return std::to_string(copper / 100) + unit;
}
```

The value and the arguments are converted to the parameters' types (bool, int,
float or `std::string` for the value; int, float or `std::string` for arguments).
An engine pipe wins a name both have, and the game is told. A pipe that does not
exist, or arguments that do not fit it, leave the value unformatted and are
reported once; a number pipe given text that is not a number leaves it as it is.

A pipe runs when the value it formats changes, not on every frame, so it should
only format: the same value always gives the same text.

A prop bound through a pipe is still stored as its declared type:
`shown="{stamina|round}"` on an `int` prop holds the number 118.

## Data flow

Values arrive through a `UiContext` (see [UiContext](UiContext.md)): the props a
widget's placement passes, its Models, its `ui().set()` calls, and its controller's
fields. Contexts nest: a widget instance resolves `{key}` in its own context first
(its values, then its controller's fields), then its parent's, up to the root. Each
repeat of a `foreach` is a context of its own under the foreach's, holding its entry
and index under the names the `foreach` gives them.

## Lists

A list is a `std::vector` field of a Model: a controller's `[[fj::model]]` struct or
a component's `[[fj::state]]` field. `foreach="slot, i in hud.slots"` stamps the element
once per entry; inside it `{slot.name}` reads the entry (a struct is read field by
field, a value as it is) and `{i}` its position from 0. The vector is read where it
is, every frame the repeats are on show, so the game changes its data and the UI
follows: a list that grows or shrinks at its end keeps the repeats it already had and
stamps or removes the rest. `if="hud.slots"` is false for an empty list. A widget
placement can be repeated the same way ([Widgets](Widgets.md#repeating-a-widget)).

Actions go the other way. A click bubbles from the hit element upward until an
element with `on:click` is found. If that element sits inside a widget instance that
maps the event, `on:click="activate"` inside `hotbar_slot.fjwidget` fires the
caller's `on:activate="select_slot(1)"` in the caller's context; otherwise the action
dispatches where it was found and bubbles up the context chain to the first handler.

## Layout

Flexbox, through Yoga. Each element has `display`, `flex-direction`,
`justify-content`, `align-items`, `flex-grow` and the rest from its stylesheet, so
the markup only nests boxes and the sheet decides direction, size and spacing.
Absolute positioning, `overflow: scroll` with mouse-wheel scrolling, and `%` /
viewport units are all in [FJSS](FJSS.md).

The root is laid out in the project's design resolution and scaled to the game
viewport, so sizes in a sheet are stable across screens; see "Scaling" in
[FJSS](FJSS.md).

## Hot-reload

Debug builds watch the markup, its sheet, every `@import` and every widget it uses.
Saving any of them reloads the tree in place, in the editor and in play mode. Hover,
focus and running transitions reset; widget controllers are recreated and their
`begin_play()` runs again. A file that fails to parse leaves the last good tree up
and reports the problem with file, line and column.
