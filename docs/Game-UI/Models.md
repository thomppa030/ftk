# Models: what a widget reads

A widget's markup reads the game where it lives. Nothing copies values into the UI
and nothing announces a change: a Model points at a component or a struct, and each
frame the UI reads what its bindings name and redraws what changed.

```html
<widget class="hud_controller">
    <model name="player" component="PlayerStats" from="local"/>
    <prop name="compact" type="bool" default="0"/>

    <text>{player.hp} / {player.max_hp}</text>
    <div class="slot" foreach="slot in hud.slots" class:active="slot.active"
         on:click="select_slot({slot.key})">
        <text>{slot.name}</text>
    </div>
</widget>
```

## Where a name comes from

The first name of a path is, in the widget's own scope:

- a **prop** the placement passes (`<prop name="compact">`), typed as it declares;
- a **Model**: a component read from a node (`<model name="player">`, see
  [Widgets](Widgets.md#models)), or a controller's `[[fj::model]]` struct;
- a **controller field**: the widget's controller's `[[fj::state]]` fields and
  getters, read in place;
- a **`foreach` name**: the entry and index of the repeat it is in.

A name the widget's scope does not hold is looked for in the scope of the widget
that placed it, and so on out to the root. The rest of the path steps into what
the first name reached: a struct's fields (`{player.stats.armor}`), a getter
(`{player.low_health}`, which runs every time it is read), an enum's label, or a
vector's component (`{player.velocity.y}`). A list is read with `foreach` (see
[FJUI](FJUI.md#lists)), and a value is formatted with pipes (`{player.hp|round}`,
see [FJUI](FJUI.md#pipes)).

## How a value reads

A value keeps its kind until something shows it, and markup reads it three ways:

- as **text** for `{…}`: a bool as `1`/`0`, a float as the shortest decimal that
  reads back as the same number, a vector as its components separated by `, `;
- as a **number** for a bar's `value` and `max`, and for an action's argument:
  text counts when it starts with one;
- as **true or false** for `if=` and `class:`: false is no value, `false`, zero, a
  vector of zeros, a Model that found nothing, an empty list, and the texts `""`,
  `"0"` and `"false"`.

`class:active="selected=={index}"` compares what the left side reads with the
text on the right. There are no expressions beyond that: a comparison or a sum
belongs in a `[[fj::state]]` getter, where the game can test it.

## The controller's side

```cpp
struct HudModel {
    std::vector<HotbarSlot> slots;
    int selected = 1;
};

class hud_controller : public fjell::UiWidget {
    FJELL_UI_WIDGET(hud_controller)
public:
    [[fj::model]] HudModel hud;              // {hud.selected}, foreach over hud.slots
    [[fj::state]] bool open = false;         // if="open"
    [[fj::prop]] bool compact = false;       // filled from <prop name="compact">
    [[fj::prop]] fjell::Ref<PlayerStats> stats; // the component <prop name="stats"> holds
    [[fj::action]] void select_slot(int key);
    void tick(float dt) override;
};
```

- **`[[fj::model]]`** marks a struct the markup reads by the field's name. Every
  public data member of the struct, and of the structs it holds, is readable; a
  `std::vector` of them is a list.
- **`[[fj::state]]`** marks a field or a const getter the markup reads by name. A
  controller changes what the UI shows by assigning to its own fields.
- **`[[fj::prop]]`** marks a field filled from the widget's prop of the same name
  before the controller starts and before every `tick()`: a `bool`, `int`, `float`
  or `std::string`, whatever the placement bound the prop to, or a `Ref<T>` to the
  component a component prop holds (`stats="{player}"` hands a nested widget the
  player's `PlayerStats`). The Ref follows the prop: it points at no component
  while the Model behind it finds none, and like any Ref it knows when its node is
  gone. The widget check reports a `[[fj::prop]]` whose `<prop>` is missing or of
  another kind. The markup reads the prop itself, so a `[[fj::prop]]` is for the
  controller's own code.
- **`[[fj::action]]`** methods answer `on:click` and a placed widget's events,
  with typed arguments (see [Widgets](Widgets.md#controllers)).

A controller reads the component a Model shows with `model<PlayerStats>("player")`
and picks the node a Model reads at runtime with `models().bind("target", enemy)`,
`enemy` a `Ref<Node3D>`.

## Change tracking

When a tree loads, every binding in it (text, `if=`, `class:`, `style-*`, `src`,
`material`, a bar, a prop, a `foreach`) is parsed once into a table. Each frame the
UI evaluates the bindings on show and compares what they read with what they read
last frame; only a binding whose value changed is applied, and a pipe runs only
then. A change that moves classes, visibility or styles triggers a relayout. A
subtree hidden by `if=` or `display: none` evaluates only its root's props, `if=`
and `class:`, so a closed menu costs almost nothing. A getter runs every frame
while something on show reads it: keep it cheap, or make it a field the game keeps
up to date.

## Lifetime

A `UiSubsystem` builds one UI per placed widget at play start and tears it down at
play end, with a scope per widget instance and per repeat. A Model is bound after
the frame's last destroy and before anything reads the UI, so a path never reads a
component that is gone; a destroyed node leaves its Model empty. A widget
hot-reload rebuilds the scopes and the controllers and runs `begin_play()` again.

The UI editor feeds the same bindings from its Data panel, without a controller
(see [Widgets](Widgets.md#previewing-with-data)), and checks every path against
what the game declares (see [Widgets](Widgets.md#checking)).
