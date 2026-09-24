# Widgets — `.fjwidget`

The MVC composition layer above FJUI. A widget bundles markup, a stylesheet, typed
props and an optional C++ controller, and can be placed many times with different
data. Raw FJUI does not compose; widgets do.

## File anatomy

```html
<widget class="health_bar_controller" style="health_bar.fjss">
    <prop name="stat" type="string" default="health"/>
    <prop name="value" type="float" default="100"/>
    <prop name="max" type="float" default="100"/>
    <prop name="color" type="string" default="#44bb44"/>

    <div class="vital-row">
        <text class="vital-label">{label}</text>
        <div class="vital-track">
            <bar class="vital-fill" value="{value}" max="{max}" style---fill="{color}"/>
        </div>
    </div>
</widget>
```

- `class` names the C++ `UiWidget` controller, `style` the sheet next to the file.
- `<prop>` declares what a placement may pass: `string`, `float`, `int`, `bool`,
  with an optional default. A placement passing an undeclared prop is warned about;
  a `{key}` in the markup that matches no prop is warned about at load.
- The rest is the markup. Widgets live in `widgets/` next to the root UI and are
  used by tag: `<health-bar stat="mana" color="#4477cc"/>`.
- The markup's own text lives here. A same-named `.fjui` beside a widget is a
  leftover from before that move; nothing reads it, and the loader says so.

## Styling a widget

A widget's sheet is scoped to each instance: its selectors match only inside that
instance, its own ancestor selectors work (`.row .label`), and it may style its own
instance node by tag. Props reach styles two ways:

- `class:active="selected=={index}"` adds a class when a condition holds; the sheet
  styles `.slot.active`.
- `style---fill="{color}"` sets a custom property from a prop; the sheet reads it
  with `var(--fill)`. Any `style-*` attribute resolves `{key}` templates.

A placement can add classes of its own: the UI Widget component's **Style Classes**
field puts them on the widget's root node, so one nameplate file serves as `.enemy`
on one node and `.friendly` on another with both variants defined once in its sheet.

## Placing a widget in a scene

A node with a **UI Widget** component and a `.fjwidget` path. Screen-space
placements scale to the project's design resolution by default (Project Settings →
Game UI); the component can choose native pixels or a custom height instead.
World-space placements draw on a quad in the scene, laid out in the component's
virtual viewport, and never scale to the design frame.

At play start the subsystem builds one `GameUi` per placement, creates the
controller named by `class` and one per nested widget, and calls `begin_play()`.
The console line for each placement names its sub-widgets, its classes and the
scaling in effect.

## Controllers

```cpp
class health_bar_controller : public fjell::UiWidget {
    FJELL_UI_WIDGET(health_bar_controller)
    [[fj::bind]] int value = 100;     // reachable as {value}
    void begin_play() override;      // bind actions, read props with ui().get("stat")
};
```

`[[fj::bind]]` fields publish themselves into the widget's context every tick;
`[[fj::action]]` methods and `ui().bind_action()` answer `on:click` and parameterized
actions such as `on:activate="select_slot(3)"`. Lists for `foreach` are published
with `ui().set_list_size()` and `ui().set_item()` (see [UiContext](UiContext.md)),
cheap enough to call every tick. Find scene components by type with
`SceneAPI::find_node_with<PlayerStats>()` rather than by node name.

## Slot projection

A `<slot/>` in the widget's markup is replaced by the children the placement writes
between its tags: `<hotbar-slot index="1"><div class="slot-icon"/></hotbar-slot>`.
That content is the caller's and is styled by the caller's sheet.

## Hot-reload and saving

Debug builds watch the widget file, its sheet, every `@import` and every nested
widget. A save rebuilds the running instance, in play mode too: controllers are
torn down and recreated, `begin_play()` runs again, and script fields pointing at
the old ones are rebound. A file that fails to parse mid-edit leaves the last good
tree on screen and reports why.

The UI editor opens a `.fjwidget` directly and previews it at the design
resolution. Ctrl+S writes the header, the `<prop>` lines and the markup back, and
the sheet next to it. `fjlint` checks widget files and their sheets from the shell.
