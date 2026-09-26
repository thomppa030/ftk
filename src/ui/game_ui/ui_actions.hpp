#pragma once

#include "ui/game_ui/ui_value.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fjell {

struct OwnedUiActionDesc;

/// An action as markup writes it: `select_slot({slot.key})`, `close`,
/// `aim(0.5, 1)`.
struct UiActionCall {
    std::string name;
    std::vector<std::string> args; ///< each as written, trimmed; `{…}` still in them
};

/// Splits `name(arg, arg)` into its name and arguments; `name` alone has none.
[[nodiscard]] UiActionCall parse_action_call(std::string_view text);

/// Calls the action on `self`, its arguments converted to what the method
/// takes: nothing; a whole number or a number (from a number, or text that is
/// one); a vec2 (from a vector, or two numbers). Empty when it ran, otherwise
/// why it did not.
[[nodiscard]] std::string call_action(const OwnedUiActionDesc& action, void* self, std::span<const UiValue> args);

/// Why a call written in markup cannot reach this action, judged before it
/// runs: the number of arguments, and a literal argument of the wrong kind.
/// Bound arguments (`{…}`) are judged when they are clicked. Empty when it can.
[[nodiscard]] std::string check_action_call(const OwnedUiActionDesc& action, const UiActionCall& call);

} // namespace fjell
