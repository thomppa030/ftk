#pragma once

#include <cstdint>
#include <exception>
#include <string>
#include <string_view>

namespace fjell {

struct Node;

/// A call from the engine into the game library: which script type, which of
/// its hooks or callbacks, on which node, and the script object itself. For
/// the report when what runs there throws.
struct ScriptCall {
    std::string_view type_name;     ///< the script's type, or what is being called
    const char* hook{""};           ///< "tick", "timer", "on:click", ...
    const Node* node{nullptr};      ///< the node it runs for, when there is one
    const void* instance{nullptr};  ///< the script object
};

/// The call running now, or null outside any.
[[nodiscard]] const ScriptCall* current_script_call();

/// "PlayerController.tick on 'Player'", for a report.
[[nodiscard]] std::string describe(const ScriptCall& call);

namespace detail {

class CurrentScriptCall {
public:
    explicit CurrentScriptCall(const ScriptCall& call);
    ~CurrentScriptCall();
    CurrentScriptCall(const CurrentScriptCall&) = delete;
    CurrentScriptCall& operator=(const CurrentScriptCall&) = delete;

private:
    const ScriptCall* previous_;
};

void report_script_exception(const ScriptCall& call, const std::exception* error);

} // namespace detail

/// Runs `fn`, which calls into the game library, as `call`. An exception it
/// throws is reported and never let out, so one script's mistake cannot take
/// the engine down. Returns whether it ran through.
template <typename Fn>
bool call_script(const ScriptCall& call, Fn&& fn) {
    const detail::CurrentScriptCall current(call);
    try {
        fn();
        return true;
    } catch (const std::exception& e) {
        detail::report_script_exception(call, &e);
    } catch (...) {
        detail::report_script_exception(call, nullptr);
    }
    return false;
}

} // namespace fjell
