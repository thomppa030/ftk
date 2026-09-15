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
| `{key}` in text or any attribute | replaced by the context value, every frame it changes |
| `if="key"` | shows the element while `key` is truthy: not empty, not `0`, not `false` |
| `class:name="key==value"` | adds `name` while the comparison holds; either side may be a `{key}` |
| `on:click="action"` | dispatches `action`; `select_slot(3)` passes arguments |
| `foreach="list"` | repeats the element once per item of a list, each item its own scope |
| `style-<property>="value"` | an inline style, templates resolved: `style---fill="{color}"` |
| `value`, `max` on `bar` | fill fraction, also `FILL` inside a ui shader |
| `settle`, `bounce` on `bar` | how the drawn fill follows the value: seconds to come to rest (0.35) and overshoot 0..1 (0.25) |
| `src`, `material` | texture to draw, ui material to paint with |

Comments are `<!-- -->`, elements self-close with `/>`, and attribute values are
double-quoted. Whitespace-only text between elements is dropped.

## Data flow

Values arrive through a `UiContext` (see [UiContext](UiContext.md)): a widget's
`[[fj::bind]]` fields, its `ui().set()` calls, and the props its placement passes.
Contexts nest: a widget instance resolves `{key}` in its own context first, then its
parent's, up to the root. A `foreach` item is a context of its own with `{__index}`
set to its position.

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
