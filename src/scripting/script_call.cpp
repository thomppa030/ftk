#include "scripting/script_call.hpp"

#include "ftk/base/log.hpp"
#include "scene/node.hpp"

#include <utility>

namespace fjell {

namespace {

// Scripts run on the main thread only.
const ScriptCall* g_current = nullptr;
bool g_halted = false;
std::optional<MissingReferenceReport> g_report;

} // namespace

const ScriptCall* current_script_call() { return g_current; }

std::string describe(const ScriptCall& call) {
    std::string where(call.type_name);
    if (call.hook != nullptr && call.hook[0] != '\0') where += std::string(".") + call.hook;
    if (call.node != nullptr) where += " on '" + call.node->name + "'";
    return where;
}

bool script_calls_halted() { return g_halted; }

void halt_script_calls(MissingReferenceReport report) {
    // The first is the one that matters; what follows from it is noise.
    if (g_halted) return;
    g_halted = true;
    g_report = std::move(report);
}

std::optional<MissingReferenceReport> take_missing_reference_report() { return std::exchange(g_report, std::nullopt); }

void resume_script_calls() {
    g_halted = false;
    g_report.reset();
}

namespace detail {

CurrentScriptCall::CurrentScriptCall(const ScriptCall& call) : previous_{g_current} { g_current = &call; }

CurrentScriptCall::~CurrentScriptCall() { g_current = previous_; }

void report_script_exception(const ScriptCall& call, const std::exception* error) {
    // A reference used with nothing there has halted everything and made its own
    // report; the throw only unwound the script.
    if (g_halted) return;
    const std::string where = describe(call);
    if (error != nullptr) {
        FTK_CORE_ERROR("Exception in {}: {}", where, error->what());
    } else {
        FTK_CORE_ERROR("Unknown exception in {}", where);
    }
}

} // namespace detail

} // namespace fjell
