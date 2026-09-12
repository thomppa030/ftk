// input_action_ref.hpp — Forward declaration for Ref<InputAction> in game scripts
#pragma once

namespace fjell {
struct InputActionDef;

/// An input action asset (`.fjaction`) — a named input like "Jump" or "Move",
/// mapped to keys, buttons, and axes in the editor so players can rebind them.
///
/// Held as `Ref<InputAction>` and assigned in the inspector, then bound to a
/// method with `InputAPI::bind()`.
///
/// @code
/// [[fj::property]] fjell::Ref<fjell::InputAction> jump;
/// @endcode
using InputAction = InputActionDef;
} // namespace fjell
