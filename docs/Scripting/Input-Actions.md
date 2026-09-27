# Guide to Input

This is the whole input chain in Fjell — from a key press in the OS to a function on your component getting called. If you're skimming, the **TL;DR** section below is enough to get unstuck; the rest is for when you want to understand what's actually happening.

> Reference for the script API surface lives in [`InputAPI`](API/InputAPI.md) and [`Input`](API/Input.md). This page is the conceptual chain that holds them together.

## TL;DR

You author input in three small files:

1. **`name.fjaction`** — declares an abstract action ("Jump", "Move"). Has a *type*: `button`, `axis1d`, or `axis2d`.
2. **`name.fjinput`** — a *mapping context* that binds those actions to hardware (W key, left stick, mouse delta).
3. **`InputComponent`** on a scene node — lists which contexts are active when the scene plays.

Then in your `ScriptComponent`:

```cpp
[[fj::property]] fjell::Ref<fjell::InputAction> jump;

void begin_play() override {
    fjell::InputAPI::bind<&Player::on_jump>(jump, fjell::EInputTrigger::Pressed, this);
}

void on_jump() { /* … */ }
```

That's the whole chain. The rest of this page explains why each piece exists, what the file formats look like, and how the runtime evaluates them.

## Why not just poll keys?

You *can* poll directly — `input().is_key_held(FJELL_KEY_W)` works. It's the right call for prototyping a single mechanic, or for editor / debug tools.

For game-facing controls you'll outgrow it fast:
- **Rebinding.** Players want to remap. Hard-coded key codes don't.
- **Multiple devices.** Keyboard, mouse, gamepad — same action, different sources.
- **Context.** The same key (Esc) does different things in the menu vs. gameplay.
- **Composite values.** WASD is conceptually a 2D vector, not four bools.

The action system solves all four. Once you've authored a `.fjaction` + `.fjinput`, the script side stops caring about hardware — it just listens for "Jump fired" or "Move = (-1, 0)".

## The four pieces

```
┌──────────────┐   ┌──────────────┐   ┌──────────────┐   ┌──────────────┐
│  .fjaction   │   │  .fjinput    │   │ InputComponent│   │  Your script │
│              │   │              │   │              │   │              │
│ "what action │   │ "which keys  │   │ "which       │   │ "call me when│
│  exists, and │   │  fire which  │   │  contexts    │   │  this action │
│  what type"  │   │  action"     │   │  are loaded" │   │  fires"      │
└──────────────┘   └──────────────┘   └──────────────┘   └──────────────┘
   asset on disk    asset on disk    scene component       runtime bind
```

### 1. `.fjaction` — what action exists

A tiny JSON file under (by default) `assets/input/actions/`. One file per action.

```json
{ "name": "jump", "type": "button" }
```

```json
{ "name": "move", "type": "axis2d" }
```

```json
{ "name": "throttle", "type": "axis1d" }
```

The `type` field is what `InputAPI::bind` keys on later. A button action calls `void()` methods; `axis1d` calls `void(float)`; `axis2d` calls `void(glm::vec2)`. Mismatch and the bind fails at compile time.

You typically write these once and forget about them. The action *name* is the contract; everything else (which hardware fires it, what context it lives in) can change without your script noticing.

### 2. `.fjinput` — mapping context

Also JSON; one file per *context* (e.g. `gameplay.fjinput`, `menu.fjinput`, `vehicle.fjinput`). A context holds a list of **bindings**, each one mapping one or more hardware sources to an action.

```json
{
    "name": "gameplay",
    "consume": true,
    "bindings": [
        {
            "action": "move",
            "up": "key_w", "down": "key_s",
            "left": "key_a", "right": "key_d"
        },
        {
            "action": "move",
            "source": "pad_lstick_x",
            "modifiers": [{ "type": "deadzone", "value": 0.15 }]
        },
        {
            "action": "move",
            "source": "pad_lstick_y",
            "modifiers": [
                { "type": "deadzone", "value": 0.15 },
                { "type": "swizzle" }
            ]
        },
        { "action": "jump", "source": "key_space" },
        { "action": "jump", "source": "pad_a"    }
    ]
}
```

A few things to notice:

- **Composite (`up`/`down`/`left`/`right`)** is the shorthand for WASD-style 2D actions. Each direction is one source; the evaluator sums them into a `vec2`, normalises if magnitude > 1.
- **Single `source`** is everything else: one button, one axis. Source names live in `input_source.hpp` — keys (`key_w`, `key_space`, `key_lshift`), mouse buttons (`mouse_left`), mouse axes (`mouse_move_x`, `mouse_scroll_y`), gamepad buttons (`pad_a`, `pad_start`), gamepad axes (`pad_lstick_x`, `pad_lt`).
- **Multiple bindings can target the same action.** Above, "move" is bound to WASD *and* the left stick; "jump" to Space *and* gamepad A. The evaluator takes the largest magnitude across all bindings.
- **`consume: true`** means: if any action in this context fires, lower-priority contexts on the stack don't see that action this frame. Use `false` for overlay contexts that should *add* to gameplay rather than replace it.

#### Modifiers

A binding can have a chain of modifiers that transform the raw source value. Applied in order. Four exist today:

| Type       | Effect                                    | Config              |
|------------|-------------------------------------------|---------------------|
| `scale`    | Multiply value                            | `value` (float)     |
| `negate`   | Flip sign                                 | none                |
| `deadzone` | Zero below threshold; remap rest to 0..1  | `value` (0–1)       |
| `swizzle`  | Swap X/Y (2D actions only)                | none                |

Typical recipe for an analog stick axis: deadzone (kill stick drift), then scale (tune sensitivity), maybe negate (invert Y).

### 3. `InputComponent` — what's active

`.fjinput` files on disk are dormant. To make them live, attach an `InputComponent` to a node in your scene (commonly the player or a `GameMode`-style root). Its inspector lets you:

- Pick which `.fjinput` files to push onto the stack, in **bottom-to-top order** — first entry in the list is lowest priority. ([source](../../src/core/input_component.hpp))
- Override `actions_dir` if your `.fjaction` files live somewhere other than `assets/input/actions/`.

On play start, the engine scans the actions directory (registering every `.fjaction`) and pushes each listed context. On play stop, it pops them. You don't manage that lifecycle yourself.

### 4. Script side — `InputAPI::bind`

Inside any `ScriptComponent`:

```cpp
class Player : public fjell::ScriptComponent {
    FJELL_COMPONENT(Player)

    [[fj::property]] fjell::Ref<fjell::InputAction> move;
    [[fj::property]] fjell::Ref<fjell::InputAction> jump;

    void begin_play() override {
        fjell::InputAPI::bind<&Player::on_move>(move, fjell::EInputTrigger::Triggered, this);
        fjell::InputAPI::bind<&Player::on_jump>(jump, fjell::EInputTrigger::Pressed,   this);
    }

    void on_move(glm::vec2 v) { move_input_ = v; }
    void on_jump()            { try_jump(); }

private:
    glm::vec2 move_input_{0};
};
```

Two `[[fj::property]] Ref<InputAction>` fields → the inspector lets you drag a `.fjaction` onto each. The bind picks the correct overload from the method signature (`void()` → button, `void(float)` → axis1d, `void(glm::vec2)` → axis2d).

**Bind in `begin_play`, every time** — bindings don't survive hot-reload. There's a global call (`InputAPI::clear_connections()`) that drops every script binding; the engine calls it for you on reload and on scene unload, so usually you just bind and forget.

## Triggers, in detail

`EInputTrigger` has four values. Their actual semantics (as of today's `input_action_map.cpp`):

| Trigger     | Fires when…                                       | Applies to        |
|-------------|---------------------------------------------------|-------------------|
| `Pressed`   | Button transitions from up → down this frame      | button only       |
| `Released`  | Button transitions from down → up this frame      | button only       |
| `Triggered` | Action transitions from inactive → active         | any action type   |
| `Completed` | Action transitions from active → inactive         | any action type   |

"Active" for a button means held; for `axis1d` it means non-zero; for `axis2d` it means `length > 0`. So if you let the stick return to centre, `Completed` fires once.

> Common confusion: `Triggered` is *not* "fires every frame the action is held." It's a single transition event. If you want a per-frame value, bind an axis callback (see below) — those fire every frame while the action is active.

### Axis callbacks are special

For `axis1d` / `axis2d` actions, the bound method (`void(float)` / `void(vec2)`) is called **every frame the action is active**, with the current value. The `trigger` argument you pass to `bind` is mostly cosmetic for axes — the only thing it adds is an extra zero-valued call on `Completed` if you ask for it. ([source](../../src/scripting/input_api.cpp))

So this is the typical pattern:

```cpp
fjell::InputAPI::bind<&Player::on_move>(move, fjell::EInputTrigger::Triggered, this);
//                                                            ^ ignored for axis2d
void on_move(glm::vec2 v) { move_input_ = v; }  // called every frame while move is non-zero
```

## Context stacks

Contexts compose. `push_context("menu")` adds it on top of whatever's already on the stack; `pop_context("menu")` removes it. Evaluation walks **top-to-bottom**, so the most recently pushed context wins on overlapping bindings (and, if it has `consume: true`, hides those actions from contexts below).

```cpp
void open_pause_menu() {
    fjell::InputAPI::push_context("menu");
}
void close_pause_menu() {
    fjell::InputAPI::pop_context("menu");
}
```

Practical pattern:
- `gameplay` context: WASD, jump, fire, look — `consume: true`.
- `menu` context pushed on top while a menu is open: navigation + Esc to close — `consume: true`. Movement keys are silently swallowed.
- A debug-tools overlay context with `consume: false` so its bindings co-exist with gameplay.

Contexts loaded via `InputComponent` are pushed at play start and popped at play stop. Anything you push manually (e.g. a menu) you're responsible for popping.

## Direct polling — when to use it

`world().input` (also exposed as `input()` inside a `ScriptComponent`) gives you the raw [`Input`](API/Input.md) state — `is_key_pressed`, `mouse_position`, `mouse_delta`, gamepad button states. No action system involved.

Reach for it when:
- You're prototyping and don't want to author files yet.
- You're writing editor / debug UI that shouldn't be rebindable.
- You need something the action system doesn't expose (raw mouse delta inside a custom camera-look implementation, for example — though many people end up routing that through an `axis2d` action too).

Avoid it for player-facing controls in shipped code.

## End-to-end: what happens when you press W

For a `move` action bound as a WASD composite in the `gameplay` context, with `on_move(vec2)` bound on `Player`:

1. **OS event** — SDL reports the key to the window, which hands the event to its `Input`. `Input::receive` flips `keys_[FJELL_KEY_W]` to `Pressed`.
2. **Per-frame promotion** — `Input::update()` runs at the top of the frame; `Pressed` becomes `Held`.
3. **Action evaluation** — `InputActionMap::update()` walks the active context stack top-down. In `gameplay`, the `move` composite binding sees `key_w` held → adds `+1` to the Y component. `state.axis = {0, 1}`, `state.held = true`.
4. **Modifiers** — none on this binding; value passes through.
5. **Multi-binding merge** — the left-stick binding for `move` also evaluates; its magnitude is 0 (stick centred), so the WASD value wins.
6. **Consume** — `gameplay` has `consume: true`, so any context below also targeting `move` is skipped this frame.
7. **Transition detection** — `was_active` was `false`, now `true`. `on_triggered` delegate fires; `on_axis2d` delegate broadcasts `{0, 1}`.
8. **Script dispatch** — your bound `Player::on_move({0, 1})` runs.

Next frame, the key is still held: steps 1–6 repeat, `was_active` is now `true`, so `on_triggered` does *not* fire again — but `on_axis2d` still broadcasts every frame. When you release W, `on_completed` fires once, then everything goes quiet.

## Authoring in the editor

The JSON above is what the engine *loads*. In practice you'll author through the **Input Mapping** editor context, which opens when you double-click (or right-click → "Open in Input Editor") a `.fjinput` file in the content browser. The same flow handles `.fjaction` files, but the editor is fundamentally a `.fjinput` editor — `.fjaction` files are picked up by scanning, not authored here yet (you create them as JSON in the content browser, or in your text editor).

### Layout

The dockspace is one big **Mappings** panel (85% of the height) on top, with the shared **History** and **Console** tabs along the bottom. The Mappings panel is the entire editor — everything below lives inside it.

### Header bar

A framed strip at the top of the Mappings panel with three controls:

- **Context name** — text field bound to the `name` field of the `.fjinput`. This is the string you pass to `push_context()` / `pop_context()`, not the filename. The editor doesn't enforce that they match; keep them in sync yourself.
- **Consume** checkbox — toggles `consume: true/false` for the whole context (tooltip: *"Consume matched inputs, blocking lower contexts"*).
- **+ Add Action** button — opens the action picker (see below). Right-aligned, with an orange **"● unsaved"** indicator next to it whenever the in-memory context has unsaved edits.

### Action cards

Each action that has at least one binding in the context gets its own **card** below the header — a rounded dark panel with:

- A **coloured left edge stripe** keyed by action type: green for buttons, blue for axis1d, orange for axis2d. Same colour as the type pill on the right of the title row, so you can scan the column visually.
- A collapsible **TreeNode header** with the action name, the type pill (`Button` / `Axis1D` / `Axis2D`), and a muted **Remove** button on the far right that drops the action and *all its bindings* from this context (no confirmation — removal is deferred to end-of-frame).
- The list of bindings for this action, each as one row.
- A **+ Binding** button at the bottom. For axis2d actions, a second **+ Composite (WASD)** button appears next to it that adds a binding pre-filled with W/A/S/D on up/down/left/right — the one-click recipe for keyboard movement.

Cards are listed in the order their actions first appear in the bindings list. Within a card, bindings keep their array order.

### Binding rows

Each binding renders as a row inside its action card:

- **Single-source bindings** show one fat button labelled with a category tag (`KB`, `M`, or `PAD`) plus the human-readable source name (e.g. `KB  Space`, `PAD  A`, `M  Move X`). The left edge of the button gets a 3-pixel stripe coloured by the source category (blue for keyboard, amber for mouse, purple for pad) so you can spot mismatches at a glance.
- **Composite bindings** (the `up`/`down`/`left`/`right` shape) show four smaller buttons in a row prefixed `UP / LF / RT / DN`, followed by a muted `composite` label so you know what shape you're looking at. Each direction is independently rebindable.
- Each button is hover-tooltipped *"<source> — click to rebind"*.
- A muted **✕** at the right end of the row removes just that binding.

Below the source row is a **Modifiers:** line with one pill per modifier, in chain order. Each pill is coloured by modifier type (blue=scale, green=deadzone, red=negate, purple=swizzle) and shows the value for ones that have one (e.g. `Scale  1.50`, `Deadzone  0.15`). Modifiers without a value (negate, swizzle) just show the name. A trailing **+** adds a new modifier (defaults to `Scale 1.0`); if a binding has no modifiers yet, the line shows *"Modifiers:  + add"* instead.

### Rebinding

Click any source button (single-source or one of the four composite directions). The button turns **amber** and reads *"press any key..."*. The editor polls raw input every frame and captures the first key, mouse button, or gamepad button press it sees — and writes it into that slot.

- **Esc** cancels the rebind without writing (handled in `handle_shortcuts`).
- Only one rebind is active at a time — starting a second click while one is in progress is ignored.
- The rebind scan covers keyboard keys 32–349 (printable + function), mouse buttons 0–4, and gamepad buttons 0–14. Mouse axes and stick axes can't be captured this way; pick them through… well, there isn't a picker yet — you set them via the JSON for now. (Known gap; the in-editor axis picker is a future addition.)

### Editing a modifier

Click a modifier pill to open a small popup:

- **Type** combo (Scale / Deadzone / Negate / Swizzle).
- **Value** drag-float (0–10, step 0.01) — only shown for Scale and Deadzone, since Negate and Swizzle don't have a value.
- **Remove modifier** button at the bottom in muted red.

Edits write back immediately; closing the popup commits nothing new.

### Add Action popup

Triggered by the **+ Add Action** button in the header. The popup shows every `.fjaction` found under `assets/input/actions/` (the path is currently hardcoded in the editor — even if your `InputComponent` overrides `actions_dir`, the picker still scans the default), grouped under **BUTTONS**, **1D AXES**, **2D AXES** headings. Each entry shows the action name with a small type pill on the right.

- A **Filter actions...** text field at the top filters live by substring (case-insensitive).
- Actions already in the context are shown disabled with *"(already added)"* appended.
- Clicking an entry adds a new empty binding for that action and closes the popup. The new binding has an unset source — you'll need to click it to start a rebind.

To add a *new* action that doesn't exist yet, you create the `.fjaction` JSON file directly (under `assets/input/actions/`) — there is no "create action" affordance in the editor today.

### Saving

The editor flips a `dirty_` flag on every change and shows the **● unsaved** indicator in the header. Save is **Ctrl+S** (the standard editor-context shortcut, not a button) and writes the in-memory context back to the original `.fjinput` path via `write_input_context`. The running game / play session re-reads the file on next reload — no live re-evaluation of an open scene yet.

### Things to know

- **No undo** for binding edits. The History panel is docked but the input editor doesn't push commands into it — every change is a direct mutation of the in-memory context. If you misclick a rebind, your only undo is closing without saving (which discards everything since the last save). This is a known limitation worth working around by saving often.
- **Action ordering is implicit.** Cards appear in the order actions first show up in the bindings array. Reordering means editing the JSON; the editor has no drag-handles.
- **Stale picker after `.fjaction` changes** — the action scan runs when the context opens and when you click **+ Add Action**, not continuously. If you add a new `.fjaction` while the editor is open, click **+ Add Action** to refresh.
- The editor is **per-`.fjinput`**: opening a second one spins up a separate editor context tab, not a multi-doc view inside one editor.

Same files as the JSON walkthrough above — the editor is just a much faster way to produce them, and is the path you should use day-to-day.

## Common pitfalls

- **Forgot to bind in `begin_play()`.** Reload silently drops your bindings. If a control stops working after Build & Reload, look here first.
- **Trigger mismatch on axes.** Picking `Pressed` for an `axis2d` callback compiles but never fires — there's no rising-edge concept for an axis. Use `Triggered` (the default) and treat the callback as continuous.
- **Two contexts both claim the same action with `consume: true`.** Only the top one runs. Either set `consume: false` on the overlay or accept the override.
- **Context name vs. file name.** The context name is the `"name"` field inside the JSON, not the filename — but `InputComponent` loads files by path. Keep them in sync; the editor will, by default.
- **Action name vs. asset path.** `Ref<InputAction>` resolves by handle, not by string. Renaming the action without updating the `.fjaction` file's `"name"` field breaks lookups silently.

## Where the code lives

| File | Role |
|---|---|
| `src/core/input.hpp/cpp` | Raw input from SDL — keys, mouse, gamepad |
| `src/core/input_source.hpp/cpp` | `InputSource` enum + name ↔ enum conversion |
| `src/core/input_modifier.hpp/cpp` | Modifier types + application |
| `src/core/input_binding.hpp` | A single binding struct |
| `src/core/input_mapping_context.hpp` | A loaded `.fjinput` |
| `src/core/input_action.hpp` | An `.fjaction` asset shape |
| `src/core/input_context_io.cpp` | `.fjaction` / `.fjinput` (de)serialisation |
| `src/core/input_action_map.cpp` | The evaluator — context stack, modifiers, trigger state machine |
| `src/core/input_component.hpp` | The scene-attached component |
| `src/scripting/input_api.cpp` | Script-side `bind` plumbing |
| `src/scripting/fjell/input.hpp` | Direct polling exposed to scripts |
| `src/ui/input_mapping_editor_context.hpp/cpp` | The editor GUI |

## See also

- [`InputAPI`](API/InputAPI.md) — reference for `bind`, `push_context`, `pop_context`
- [`Input`](API/Input.md) — reference for direct polling
- [`Ref`](API/Ref.md) — how `Ref<InputAction>` resolves
- [Scripting Overview](Overview.md) — where `[[fj::property]]` and the reload model are explained
