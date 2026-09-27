# fjell::Input

Direct keyboard / mouse polling. For most games, prefer [`InputAPI`](InputAPI.md) — `Input` is the polling escape hatch.

| | |
|---|---|
| Header | `fjell/input.hpp` |
| Source | `src/core/input.cpp` (engine side) |

## Syntax

```cpp
namespace fjell {

class Input {
public:
    [[nodiscard]] bool is_key_pressed (int key) const;
    [[nodiscard]] bool is_key_held    (int key) const;
    [[nodiscard]] bool is_key_released(int key) const;

    [[nodiscard]] bool is_mouse_button_pressed(int button) const;
    [[nodiscard]] bool is_mouse_button_held   (int button) const;

    [[nodiscard]] glm::dvec2 mouse_position() const;
    [[nodiscard]] glm::dvec2 mouse_delta()    const;
    [[nodiscard]] glm::dvec2 scroll_delta()   const;

    [[nodiscard]] bool is_cursor_captured() const;
};

}
```

## Remarks

A snapshot of the current frame's input state, owned by the engine. Get a reference via [`ScriptComponent::input()`](ScriptComponent.md#scriptcomponentinput) or `world()->input()`. The reference is valid until the next frame. `Input` cannot be copied: hold the reference, never a value.

Every call goes to the engine's own code, so a game never depends on how the engine stores the state. The key, button, delta and scroll queries read idle or zero while the editor rather than the game has focus in docked play.

Key and mouse codes are GLFW values — `fjell/api.h` defines named constants like `FJELL_KEY_W`, `FJELL_KEY_SPACE`, `FJELL_MOUSE_LEFT`.

State distinguishes:
- **Pressed** — true for the single frame the key transitioned to down.
- **Held** — true for every frame the key is down (including the press frame).
- **Released** — true for the single frame the key transitioned to up.

`Input` is for prototyping and debug controls. For game-facing controls (rebinding, gamepad, context switching), see [Input Actions](../Input-Actions.md).

## Functions

### Keyboard

| Name | Description |
|---|---|
| [`is_key_pressed`](#inputis_key_pressed) | True on the down edge |
| [`is_key_held`](#inputis_key_held) | True while held |
| [`is_key_released`](#inputis_key_released) | True on the up edge |

### Mouse

| Name | Description |
|---|---|
| [`is_mouse_button_pressed`](#inputis_mouse_button_pressed) | True on the down edge |
| [`is_mouse_button_held`](#inputis_mouse_button_held) | True while held |
| [`mouse_position`](#inputmouse_position) | Cursor position in window pixels |
| [`mouse_delta`](#inputmouse_delta) | Cursor movement since last frame |
| [`scroll_delta`](#inputscroll_delta) | Scroll wheel movement this frame |

### Cursor

| Name | Description |
|---|---|
| [`is_cursor_captured`](#inputis_cursor_captured) | Whether cursor lock is active |

Locking the cursor is [`InputAPI::set_cursor_locked()`](InputAPI.md#inputapiset_cursor_locked).

---

## Input::is_key_pressed

### Syntax
```cpp
[[nodiscard]] bool is_key_pressed(int key) const;
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `key` | `int` | `FJELL_KEY_*` constant from `fjell/api.h`. |

### Return value
`true` exactly on the frame the key transitions from up to down. Repeats only if you release and press again.

---

## Input::is_key_held

### Syntax
```cpp
[[nodiscard]] bool is_key_held(int key) const;
```

### Return value
`true` for every frame the key is down — including the press frame and the frame before release.

### Example
```cpp
if (input().is_key_held(FJELL_KEY_W)) {
    transform().position += forward_ * speed * dt;
}
```

---

## Input::is_key_released

### Syntax
```cpp
[[nodiscard]] bool is_key_released(int key) const;
```

### Return value
`true` exactly on the frame the key transitions from down to up.

---

## Input::is_mouse_button_pressed

### Syntax
```cpp
[[nodiscard]] bool is_mouse_button_pressed(int button) const;
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `button` | `int` | `FJELL_MOUSE_LEFT` / `FJELL_MOUSE_RIGHT` / `FJELL_MOUSE_MIDDLE`. |

### Return value
True on the down edge.

---

## Input::is_mouse_button_held

### Syntax
```cpp
[[nodiscard]] bool is_mouse_button_held(int button) const;
```

True while the button is held.

---

## Input::mouse_position

### Syntax
```cpp
[[nodiscard]] glm::dvec2 mouse_position() const;
```

### Return value
Cursor position in window pixels (top-left origin, Y down).

---

## Input::mouse_delta

### Syntax
```cpp
[[nodiscard]] glm::dvec2 mouse_delta() const;
```

### Return value
Cursor movement since the last frame, in pixels. Useful for camera look-around. While the cursor is locked ([`InputAPI::set_cursor_locked(true)`](InputAPI.md#inputapiset_cursor_locked)), this is the raw delta — the cursor itself doesn't move on screen.

---

## Input::scroll_delta

### Syntax
```cpp
[[nodiscard]] glm::dvec2 scroll_delta() const;
```

### Return value
Scroll wheel movement this frame. `y` is the standard vertical scroll; `x` is horizontal scroll if the device provides it. Most users will only see `y`.

---

## Input::is_cursor_captured

### Syntax
```cpp
[[nodiscard]] bool is_cursor_captured() const;
```

True if the cursor is currently locked (hidden + delta-only). The same state as [`InputAPI::cursor_locked()`](InputAPI.md#inputapicursor_locked).

## Example

```cpp
void tick(float dt) override {
    // FPS mouselook
    if (!input().is_cursor_captured()) {
        if (input().is_mouse_button_pressed(FJELL_MOUSE_LEFT)) {
            fjell::InputAPI::set_cursor_locked(true);
        }
    } else {
        if (input().is_key_pressed(FJELL_KEY_ESCAPE)) {
            fjell::InputAPI::set_cursor_locked(false);
        }

        glm::dvec2 d = input().mouse_delta();
        yaw_   -= d.x * mouse_sens_;
        pitch_ -= d.y * mouse_sens_;
    }

    // Debug damage key
    if (input().is_key_pressed(FJELL_KEY_F)) {
        take_damage(10);
    }
}
```

## See also
- [`InputAPI`](InputAPI.md) — the production input path (action mapping) and cursor locking
- `fjell/api.h` — key code constants
