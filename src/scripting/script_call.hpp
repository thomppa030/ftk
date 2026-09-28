#pragma once

#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>

namespace fjell {

struct Node;

/// A call from the engine into the game library: which script type, which of
/// its hooks or callbacks, on which node, and the script object itself. For
/// the report when what runs there throws, or uses a reference with nothing
/// there.
struct ScriptCall {
    std::string_view type_name;     ///< the script's type, or what is being called
    const char* hook{""};           ///< "tick", "timer", "on:click", ...
    const Node* node{nullptr};      ///< the node it runs for, when there is one
    const void* instance{nullptr};  ///< the script object, to find the field a reference is in
};

/// What stopped the game: a Ref or an Asset used with nothing there.
struct MissingReferenceReport {
    std::string message;
    /// The node whose script it was, for the editor to select; 0 for none.
    uint32_t node_id{0};
};

/// The call running now, or null outside any.
[[nodiscard]] const ScriptCall* current_script_call();

/// "PlayerController.tick on 'Player'", for a report.
[[nodiscard]] std::string describe(const ScriptCall& call);

/// Whether a Ref or an Asset has been used with nothing there, and nothing more
/// runs in the game library until the engine has stopped the game.
[[nodiscard]] bool script_calls_halted();

/// Records the report and halts; see script_calls_halted().
void halt_script_calls(MissingReferenceReport report);

/// The report, once, for the engine to stop the game with. The halt stays
/// until resume_script_calls().
[[nodiscard]] std::optional<MissingReferenceReport> take_missing_reference_report();

/// Lets the game library run again, as a new play session starts.
void resume_script_calls();

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
/// the engine down; a reference used with nothing there has already halted
/// the game library and is not reported twice. While halted, `fn` does not
/// run. Returns whether it ran through.
template <typename Fn>
bool call_script(const ScriptCall& call, Fn&& fn) {
    if (script_calls_halted()) return false;
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
