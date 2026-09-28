# fjell::input

Binds input actions (`.fjaction`) to member functions and switches mapping contexts.

| | |
|---|---|
| Header | `fjell/input_api.hpp` |
| Source | `src/scripting/input_api.cpp` |

## Syntax

```cpp
namespace fjell {

enum class EInputTrigger { Triggered, Pressed, Released, Completed };

namespace input {
    template <auto Method, typename T>
    void bind(const Asset<InputAction>& action,
              EInputTrigger trigger,
              T* instance);

    void push_context(const char* name);
    void pop_context (const char* name);
    void clear_connections();

    void set_cursor_locked(bool locked);
    [[nodiscard]] bool cursor_locked();
}

}
```

## Remarks

The action system is the production input path. `.fjaction` files define abstract actions ("Jump", "Move"); `.fjinput` mapping contexts bind those actions to physical sources (keys, mouse, mouse axes). See [Input Actions](../Input-Actions.md) for the authoring side.

`bind` resolves the right overload from the bound method's signature. The method type chooses the action type — you don't pass an action type enum, the engine deduces it:

| Method signature | Action type |
|---|---|
| `void f()` | Button |
| `void f(float)` | 1D axis |
| `void f(glm::vec2)` | 2D axis |

Trying to bind a method whose signature doesn't match any overload fails at compile time.

Bindings are stored engine-side, keyed by `(action handle, trigger, instance pointer)`. They are **not** automatically cleared when your component is destroyed — call `input::clear_connections()` on shutdown if you don't manage individual bindings.

## Types

### EInputTrigger

```cpp
enum class EInputTrigger { Triggered, Pressed, Released, Completed };
```

| Value | When |
|---|---|
| `Triggered` | Fires every frame the action is active. Default — best for continuous axes. |
| `Pressed` | Fires once when the action becomes active (rising edge). |
| `Released` | Fires once when the action becomes inactive (falling edge). |
| `Completed` | Fires once when the action becomes inactive *after* having been active. Subtle difference from `Released`: ignores actions cancelled by other modifiers. |

## Functions

| Name | Description |
|---|---|
| [`bind`](#inputbind) | Bind a member function to an action + trigger |
| [`push_context`](#inputpush_context) | Activate a mapping context |
| [`pop_context`](#inputpop_context) | Deactivate a mapping context |
| [`clear_connections`](#inputclear_connections) | Drop all script bindings |
| [`set_cursor_locked`](#inputset_cursor_locked) | Lock and hide the cursor for mouse-look |
| [`cursor_locked`](#inputcursor_locked) | Whether the cursor is locked right now |

---

## input::bind

### Syntax (three overloads)

```cpp
// Button (no args)
template <auto Method, typename T>
    requires requires(T* t) { (t->*Method)(); }
void bind(const Asset<InputAction>& action, EInputTrigger trigger, T* instance);

// 1D axis (float)
template <auto Method, typename T>
    requires requires(T* t) { (t->*Method)(0.0f); }
void bind(const Asset<InputAction>& action, EInputTrigger trigger, T* instance);

// 2D axis (vec2)
template <auto Method, typename T>
    requires requires(T* t) { (t->*Method)(glm::vec2{}); }
void bind(const Asset<InputAction>& action, EInputTrigger trigger, T* instance);
```

### Template parameters
| Name | Description |
|---|---|
| `Method` | Pointer-to-member-function on `T`. Signature determines the overload. |
| `T` | Owning type. Deduced from `instance`. |

### Parameters
| Name | Type | Description |
|---|---|---|
| `action` | `const Asset<InputAction>&` | The action to listen for. One that is unset or failed to load stops the game (see [`Asset`](Asset.md)). |
| `trigger` | `EInputTrigger` | When to fire — see the [trigger table](#einputtrigger). |
| `instance` | `T*` | Object the method is called on. Captured by pointer; must outlive the binding. |

### Remarks
The engine stores a small trampoline that casts `void* self` back to `T*` and calls the member. There is no allocation per fire.

Bindings are additive — calling `bind` multiple times for the same `(action, trigger)` adds multiple listeners; all fire when the action triggers.

Typical pattern is to bind in `begin_play()`:

```cpp
void begin_play() override {
    fjell::input::bind<&PlayerController::on_move>(
        move_action, fjell::EInputTrigger::Triggered, this);
    fjell::input::bind<&PlayerController::on_jump>(
        jump_action, fjell::EInputTrigger::Pressed, this);
}
```

---

## input::push_context

### Syntax
```cpp
void push_context(const char* name);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `name` | `const char*` | Name of a registered mapping context (matches the filename of a `.fjinput` minus extension). Must not be null. |

### Remarks
Mapping contexts stack. Pushing "menu" while "gameplay" is already active means *both* are live, with the most-recently-pushed taking priority for overlapping bindings. Use `pop_context` in matching pairs.

If `name` doesn't match a known context, the call is a no-op (a warning is logged).

---

## input::pop_context

### Syntax
```cpp
void pop_context(const char* name);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `name` | `const char*` | Name to remove from the active stack. |

### Remarks
Removes the most recent push of `name`. If `name` isn't on the stack the call is a no-op.

---

## input::clear_connections

### Syntax
```cpp
void clear_connections();
```

### Remarks
Drops every script binding registered via `bind`. Use sparingly — typically only at world-level transitions (level reload, returning to main menu). It does *not* affect mapping context state.

---

## input::set_cursor_locked

### Syntax
```cpp
void set_cursor_locked(bool locked);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `locked` | `bool` | `true` to lock and hide the cursor, `false` to restore the normal cursor. |

### Remarks
Hides the cursor and frees it from the window edge, which a mouse-look needs: an unlocked cursor stops producing movement once it reaches the side of the screen. Lock it when mouse-look starts and unlock it for menus, so the player gets a pointer back. [`Input::mouse_delta()`](Input.md#inputmouse_delta) keeps reporting movement while locked.

The lock lifts by itself while the editor holds focus (docked play) and comes back when the game does, so a script locks once and does not have to track focus. Stopping play clears it.

---

## input::cursor_locked

### Syntax
```cpp
[[nodiscard]] bool cursor_locked();
```

### Return value
Whether the cursor is locked right now. This is the state in force, not the request: it reads `false` while the editor has focus even though the game asked for a lock, so a script that must know what it asked for should remember that itself.

## Example

```cpp
class PlayerController : public fjell::ScriptComponent {
    FJELL_COMPONENT(PlayerController)

    [[fj::property]] fjell::Asset<fjell::InputAction> move_action;
    [[fj::property]] fjell::Asset<fjell::InputAction> jump_action;

    void begin_play() override {
        fjell::input::bind<&PlayerController::on_move>(
            move_action, fjell::EInputTrigger::Triggered, this);
        fjell::input::bind<&PlayerController::on_jump>(
            jump_action, fjell::EInputTrigger::Pressed, this);
    }

    void on_move(glm::vec2 v) { move_input_ = v; }
    void on_jump() { /* ... */ }

private:
    glm::vec2 move_input_{0};
}
```

Pause menu context swap:

```cpp
void open_menu() { fjell::input::push_context("menu"); }
void close_menu() { fjell::input::pop_context("menu"); }
```

## See also
- [Input Actions](../Input-Actions.md) — authoring `.fjaction` / `.fjinput`
- [`Input`](Input.md) — direct polling (for one-off prototyping)
- [`Asset`](Asset.md) — how `Asset<InputAction>` is resolved
