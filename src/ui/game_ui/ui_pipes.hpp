#pragma once

#include "ui/game_ui/ui_value.hpp"

#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace fjell {

class ReflectedTypes;

/// One pipe in a binding: `{value|name:arg:arg}`.
struct UiPipeCall {
    std::string name;
    std::vector<std::string> args; ///< literal text, `'quotes'` already removed
};

/// What is inside a binding's braces: the value's name and the pipes that
/// format it, left to right.
struct UiExpression {
    std::string path;
    std::vector<UiPipeCall> pipes;
};

/// Split `hp|fixed:1|times` into its path and pipes. An argument is trimmed
/// unless it is in single quotes, which keep its spaces.
[[nodiscard]] UiExpression parse_expression(std::string_view text);

/// The pipes markup may use. The engine's (`round`, `fixed:n`, `seconds`,
/// `times`, `percent[:n]`) always; the game module's [[fj::pipe]] functions
/// once it has loaded. An engine pipe wins a name both have.
class UiPipes {
public:
    /// The module's pipes; null when none is loaded. The table is read on
    /// every call, so a reload that refills it needs no second call.
    void set_game_pipes(const ReflectedTypes* types);

    /// The value formatted by one pipe. An unknown pipe, or arguments that do
    /// not fit it, leaves the value as it is and is reported once; without a
    /// game module a pipe the engine lacks is left alone unreported.
    [[nodiscard]] UiValue apply(const UiValue& value, const UiPipeCall& call) const;

    [[nodiscard]] static bool is_engine_pipe(std::string_view name);

private:
    [[nodiscard]] bool call_game_pipe(const UiValue& value, const UiPipeCall& call, UiValue& out) const;
    void report(const std::string& message) const;

    const ReflectedTypes* game_{nullptr};
    mutable std::unordered_set<std::string> reported_;
};

} // namespace fjell
