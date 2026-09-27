// input_api.hpp — Static input action API for game scripts
#pragma once

#include "fjell/api.h"
#include "fjell/input_action_ref.hpp"
#include "fjell/asset.hpp"

#include <glm/glm.hpp>

extern "C" {
FJELL_SDK_EXPORT void fjell_input_bind_void(uint32_t action_handle_id, int trigger,
                            void* instance, void (*fn)(void*));
FJELL_SDK_EXPORT void fjell_input_bind_float(uint32_t action_handle_id, int trigger,
                             void* instance, void (*fn)(void*, float));
FJELL_SDK_EXPORT void fjell_input_bind_vec2(uint32_t action_handle_id, int trigger,
                            void* instance, void (*fn)(void*, float, float));
FJELL_SDK_EXPORT void fjell_input_clear_connections();
FJELL_SDK_EXPORT void fjell_input_push_context(const char* name);
FJELL_SDK_EXPORT void fjell_input_pop_context(const char* name);
FJELL_SDK_EXPORT int fjell_input_active_device();
FJELL_SDK_EXPORT bool fjell_input_pad_connected();
FJELL_SDK_EXPORT void fjell_input_set_cursor_locked(bool locked);
FJELL_SDK_EXPORT bool fjell_input_cursor_locked();
}

namespace fjell {

/// Binds input actions to member functions. Actions are authored as `.fjaction`
/// assets and mapped to keys, buttons, and axes in the input editor, so
/// bindings stay remappable without touching code.
///
/// Bind in `begin_play()`. Connections are dropped when play stops or scripts
/// reload; there is no per-binding unbind.
///
/// @code
/// [[fj::property]] fjell::Asset<fjell::InputAction> move;   // axis2d
/// [[fj::property]] fjell::Asset<fjell::InputAction> jump;   // button
///
/// void begin_play() override {
///     using fjell::EInputTrigger;
///     fjell::input::bind<&Player::on_move>(move, EInputTrigger::Triggered, this);
///     fjell::input::bind<&Player::on_jump>(jump, EInputTrigger::Pressed, this);
/// }
/// void on_move(glm::vec2 dir) { /* ... */ }
/// void on_jump() { /* ... */ }
/// @endcode
namespace input {
/// Bind a `void()` method — the form for button actions.
/// @tparam Method  Member function pointer, as a template argument.
/// @param action   Action asset, normally a `[[fj::property]] Asset<InputAction>`
///                 assigned in the inspector. An unassigned ref binds nothing.
/// @param trigger  When to fire: `Pressed` on press, `Released` on release,
///                 `Triggered` every frame the action is active, `Completed`
///                 when it goes inactive.
/// @param instance Object to call on — normally `this`, whose going away
///                 (its node destroyed, play stopped, scripts reloaded)
///                 releases the binding. Any other object must outlive play.
template <auto Method, typename T>
    requires requires(T* t) { (t->*Method)(); }
inline void bind(const Asset<InputAction>& action, EInputTrigger trigger, T* instance) {
    if (action.data_.handle_id == 0) return;
    fjell_input_bind_void(
        action.data_.handle_id, static_cast<int>(trigger), instance,
        [](void* self) { (static_cast<T*>(self)->*Method)(); });
}

/// Bind a `void(float)` method — the form for 1D axis actions such as a
/// trigger pull or a throttle. The argument is the axis value.
/// Overload selection follows the method's signature; the arguments are
/// otherwise identical to the `void()` form.
template <auto Method, typename T>
    requires requires(T* t) { (t->*Method)(0.0f); }
inline void bind(const Asset<InputAction>& action, EInputTrigger trigger, T* instance) {
    if (action.data_.handle_id == 0) return;
    fjell_input_bind_float(
        action.data_.handle_id, static_cast<int>(trigger), instance,
        [](void* self, float v) { (static_cast<T*>(self)->*Method)(v); });
}

/// Bind a `void(glm::vec2)` method — the form for 2D axis actions such as
/// movement or look. The argument is the stick/WASD vector.
/// Pair with `EInputTrigger::Triggered` to receive it every frame.
///
/// @warning A mouse-motion source and a stick source deliver different
///          kinds of quantity through this one callback, and a look action
///          bound to both has to scale them separately. A mouse delta is a
///          displacement in pixels — already independent of frame rate, so
///          multiplying it by `dt` makes it frame-rate dependent, which is
///          the opposite of the intent. A stick is a rate and must be
///          multiplied by `dt`. Use `active_device()` to tell them apart.
template <auto Method, typename T>
    requires requires(T* t) { (t->*Method)(glm::vec2{}); }
inline void bind(const Asset<InputAction>& action, EInputTrigger trigger, T* instance) {
    if (action.data_.handle_id == 0) return;
    fjell_input_bind_vec2(
        action.data_.handle_id, static_cast<int>(trigger), instance,
        [](void* self, float x, float y) {
            (static_cast<T*>(self)->*Method)(glm::vec2{x, y});
        });
}

/// Push an input context, letting a named mapping layer take priority —
/// e.g. push "menu" when a pause screen opens so gameplay bindings stop
/// firing. Contexts stack; push and pop in matching pairs.
inline void push_context(const char* name) { fjell_input_push_context(name); }

/// Pop a context pushed with `push_context()`, restoring the layer beneath.
/// @param name Must match the pushed name.
inline void pop_context(const char* name) { fjell_input_pop_context(name); }

/// The device the player is currently using. Bindings for both keep
/// working regardless — this says which one to show prompts for, and
/// lets gameplay tune itself (sensitivity, aim assist, camera feel).
[[nodiscard]] inline EInputDevice active_device() {
    return static_cast<EInputDevice>(fjell_input_active_device());
}

/// Whether a gamepad is connected at all, regardless of which device the
/// player last used.
[[nodiscard]] inline bool pad_connected() { return fjell_input_pad_connected(); }

/// Hide the cursor and free it from the window edge, which a mouse-look
/// needs — an unlocked cursor stops producing movement once it reaches the
/// side of the screen. Lock it when mouse-look starts and unlock it for
/// menus, so the player gets a pointer back.
///
/// The lock lifts by itself while the editor holds focus (docked play) and
/// comes back when the game does, so a script locks once and does not have
/// to track focus. Stopping play clears it.
///
/// @param locked true to lock and hide, false to restore the normal cursor.
inline void set_cursor_locked(bool locked) {
    fjell_input_set_cursor_locked(locked);
}

/// Whether the cursor is locked right now. This is the state in force, not
/// the request: it reads false while the editor has focus even though the
/// game asked for a lock, so a script that must know what it asked for
/// should remember that itself.
[[nodiscard]] inline bool cursor_locked() { return fjell_input_cursor_locked(); }
} // namespace input

} // namespace fjell
