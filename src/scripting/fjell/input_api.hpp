// input_api.hpp — Static input action API for game scripts
#pragma once

#include "fjell/api.h"
#include "fjell/input_action_ref.hpp"
#include "fjell/ref.hpp"

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
/// [[fj::property]] fjell::Ref<fjell::InputAction> move;   // axis2d
/// [[fj::property]] fjell::Ref<fjell::InputAction> jump;   // button
///
/// void begin_play() override {
///     using fjell::EInputTrigger;
///     fjell::InputAPI::bind<&Player::on_move>(move, EInputTrigger::Triggered, this);
///     fjell::InputAPI::bind<&Player::on_jump>(jump, EInputTrigger::Pressed, this);
/// }
/// void on_move(glm::vec2 dir) { /* ... */ }
/// void on_jump() { /* ... */ }
/// @endcode
struct InputAPI {
    /// Bind a `void()` method — the form for button actions.
    /// @tparam Method  Member function pointer, as a template argument.
    /// @param action   Action asset, normally a `[[fj::property]] Ref<InputAction>`
    ///                 assigned in the inspector. An unassigned ref binds nothing.
    /// @param trigger  When to fire: `Pressed` on press, `Released` on release,
    ///                 `Triggered` every frame the action is active, `Completed`
    ///                 when it goes inactive.
    /// @param instance Object to call on — normally `this`. It must outlive
    ///                 play, since bindings are not released individually.
    template <auto Method, typename T>
        requires requires(T* t) { (t->*Method)(); }
    static void bind(const Ref<InputAction>& action, EInputTrigger trigger, T* instance) {
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
    static void bind(const Ref<InputAction>& action, EInputTrigger trigger, T* instance) {
        if (action.data_.handle_id == 0) return;
        fjell_input_bind_float(
            action.data_.handle_id, static_cast<int>(trigger), instance,
            [](void* self, float v) { (static_cast<T*>(self)->*Method)(v); });
    }

    /// Bind a `void(glm::vec2)` method — the form for 2D axis actions such as
    /// movement or look. The argument is the stick/WASD vector.
    /// Pair with `EInputTrigger::Triggered` to receive it every frame.
    template <auto Method, typename T>
        requires requires(T* t) { (t->*Method)(glm::vec2{}); }
    static void bind(const Ref<InputAction>& action, EInputTrigger trigger, T* instance) {
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
    static void push_context(const char* name) { fjell_input_push_context(name); }

    /// Pop a context pushed with `push_context()`, restoring the layer beneath.
    /// @param name Must match the pushed name.
    static void pop_context(const char* name) { fjell_input_pop_context(name); }

    /// The device the player is currently using. Bindings for both keep
    /// working regardless — this says which one to show prompts for, and
    /// lets gameplay tune itself (sensitivity, aim assist, camera feel).
    [[nodiscard]] static EInputDevice active_device() {
        return static_cast<EInputDevice>(fjell_input_active_device());
    }

    /// Whether a gamepad is connected at all, regardless of which device the
    /// player last used.
    [[nodiscard]] static bool pad_connected() { return fjell_input_pad_connected(); }
};

} // namespace fjell
