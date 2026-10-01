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
  or a struct or component the game reflects (`type="HotbarSlot"`), with an optional
  default. What a widget names that nothing declares is reported by the widget
  check (see Checking).
- `<model>` names a game component the widget reads in place (see Models).
- The rest is the markup. Widgets live in `widgets/` next to the root UI and are
  used by tag: `<health-bar stat="mana" color="#4477cc"/>`.
- The markup's own text lives here. A same-named `.fjui` beside a widget is a
  leftover from before that move; nothing reads it, and the loader says so.

## Models

A Model is a game component the widget reads directly, with nothing copying its
values into the UI:

```html
<widget style="vitals.fjss">
    <model name="player" component="PlayerStats" from="local"/>
    <text>{player.hp} / {player.max_hp}</text>
    <div class="vitals" class:low="player.low_health"/>
</widget>
```

`component` is a script component's type, and `{player.hp}` a path into it: fields
marked `[[fj::state]]` or `[[fj::property]]`, `[[fj::state]]` getters, nested structs
(`{player.stats.armor}`), enum labels and vector components (`{player.velocity.y}`).
A path is read from the component where it lives, as it is now; a getter runs every
time it is read. A prop may carry a whole Model on to a nested widget:
`<vitals-panel stats="{player}"/>`.

`from` picks the node the component is read from:

- `self` (the default) is the node the widget is placed on: a nameplate reading
  its own unit.
- `local` is the node carrying that component which the local player controls; in
  a single-player game, the first node that has one.

A placement can choose too. Under **Models**, the UI Widget component lists the
widget's own `<model>`s, each with a node field showing its rule ("Local player",
"This node") until a node is picked from the scene or dragged in from the
hierarchy; only nodes carrying the Model's component are offered, and clearing the
field goes back to the rule. The pick is saved with the scene. A nested widget's
Models are not listed: it gets its data through props.

A controller can choose a Model's node at runtime, for a target frame showing
whatever the player has targeted: `models().bind("target", enemy)`, with a
`Ref<Node3D>`, reads that node until `models().clear("target")` returns the Model to
its `from` rule. A node that is gone when bound stops the game, as any `Ref` does; one
destroyed later leaves the Model empty. A controller's choice wins over the
placement's, which wins over the rule. `model<PlayerStats>("player")` hands the
controller the component its markup shows now, read-only, or null.

A controller's own `[[fj::model]]` fields are Models too, by their field name:
`[[fj::model]] HudModel hud;` is `{hud.selected}` in the markup, read in place from
the controller.

While nothing is found the Model is empty: `if="player"` is false and its paths show
nothing. A `local` Model is found by one walk over the scene and then kept. It is
looked for again when its node is destroyed or changes hands, and, while nothing is
found, only once nodes have been added, destroyed or given a script since the last
look, never every frame. A component that is not a loaded script component is
reported once, with the file and line of its `<model>`.

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
class hud_controller : public fjell::UiWidget {
    FJELL_UI_WIDGET(hud_controller)
public:
    [[fj::model]] HudModel hud;                    // {hud.selected_name}, foreach over hud.slots
    [[fj::action]] void select_slot(int slot);     // on:click="select_slot({slot.key})"
    void tick(float dt) override;
};
```

The markup reads the controller where it is: its `[[fj::model]]` structs and its
`[[fj::state]]` fields and getters, by name, each time it shows them. A prop of the
same name hides such a field and is reported. `[[fj::prop]]` fields are the other
way round: filled from the widget's props, for the controller's own code (see
[Models](Models.md#the-controllers-side)).

`[[fj::action]]` methods answer `on:click` and a widget's events: `on:click="close"`
calls `void close()`, and `on:activate="select_slot({slot.key})"` calls `void
select_slot(int)` with the int the path reads. A method takes nothing, an `int`, a
`float` or a `glm::vec2` (a vector, or two numbers), and may be private. The nearest
controller up from the click that declares the action answers it. An action no
controller around it declares, or a literal argument of the wrong kind
(`select_slot(three)`), is reported with its file and line when the widget loads; a
bound argument that does not convert is reported once when clicked. A list for
`foreach` is a `std::vector` in a `[[fj::model]]` struct (see
[FJUI](FJUI.md#lists)). Find scene components by type with
`scene::find<PlayerStats>()` rather than by node name.

## Repeating a widget

A placement may carry `foreach`, and each repeat is then a widget of its own:

```html
<hotbar-slot foreach="slot in hud.slots" item="{slot}" on:activate="select_slot({slot.key})"/>
```

Each repeat gets its own scope, under the repeat's, so its props read the entry
(`item="{slot}"`) and its markup reads its props. Its Models are its own, and so is
its controller when the widget has one: created once the repeat is stamped and
bound, destroyed before the repeat is removed, so a list that shrinks takes the
controllers of the entries it lost with it. Those entries are gone from the game's
vector by then, so in that controller's `end_play` its entry, and any prop bound
from it, reads as nothing. The event it raises (`activate`) is
answered in the placing widget, with arguments read in the repeat's scope.

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
resolution. Ctrl+S writes the header, the `<prop>` and `<model>` lines and the markup back, and
the sheet next to it.

## Previewing with data

The preview is fed the way the game will feed it, from the **Data** panel beside
Properties. Nothing there is saved; it lasts while the editor has the widget open,
and survives an edit to the file for every name still declared the same way.

- **A component Model**, and a prop typed as a component, is read from the open
  scene: by its `from` rule, or from a node picked in its Reads field. A `self`
  Model reads the node whose UI Widget component places this widget; a component
  prop starts on the local player's node. Its values are shown and not edited
  (they are the scene's), and while the scene plays they move with the game.
- **A controller's Model** (`[[fj::model]]`), and a prop typed as a struct, is built
  from its type's defaults and edited in place: numbers, text, and lists, whose
  entries are added, removed and dragged into order. Reset puts it back to the
  defaults. The preview runs no controller, so nothing fills it but you.
- **A value prop** starts from its `default=`.

The preview answers every action itself: the Action log lists each one with the
arguments a controller would be given, and nothing else happens. Build & Reload
builds the Models again from the new build's defaults, since their layout may have
changed.

## Checking

A widget is checked against what its game declares, as fjreflect writes it to
`scripts/generated/manifest.json` on every build. The check reports, at the file,
line and column where it is written:

- a path whose first name is no prop, Model, controller field or `foreach` name,
  or whose next name the type it reaches does not have (`'PlayerStats' has no
  field 'low_hp'`), including a field read off a list outside a `foreach` and a
  vector component that is not there;
- a `foreach` over something that is not a list;
- a prop typed neither as a value nor as a reflected type, a Model whose
  component is not a script component, a controller that is not a `UiWidget`;
- a pipe neither the engine nor the game has, or given the wrong number of
  arguments;
- an action no controller around it declares, or a literal argument of the wrong
  kind (`select_slot(one)`);
- on a placement: a prop the widget does not declare or of the wrong type, an
  event the widget does not raise, an event no controller answers and the
  placement does not map, and a tag that is neither an element nor a widget in
  `widgets/`.

A click in a widget under `widgets/` that its own controller does not answer is
an event for whoever places it, and is checked at each placement. A game that
has not been built has no manifest: its widgets are checked against their own
props alone.

The editor checks every widget in the project when the project opens and logs
what it finds to the console. Each time the game library loads again (Build &
Reload, or an automatic build after a script is saved) it checks them against the
new manifest, and logs the problems of the widgets that name a type or game pipe
the build declares differently: rename a field, and the widgets that read it say
so. The UI editor's Problems list shows the open widget's problems and those of
every widget it places, beside the stylesheet's, checked again on open, on save,
on a change to the file on disk and after each build.

`fjlint` runs the check from the shell, finding the manifest by walking up from
each widget, along with the widget's sheet:

```bash
./build/tools/fjlint/fjlint assets/ui
```
