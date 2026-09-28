#include "scripting/script_call.hpp"

#include "core/log.hpp"
#include "scene/node.hpp"


namespace fjell {

namespace {

// Scripts run on the main thread only.
const ScriptCall* g_current = nullptr;

} // namespace

const ScriptCall* current_script_call() { return g_current; }

std::string describe(const ScriptCall& call) {
    std::string where(call.type_name);
    if (call.hook != nullptr && call.hook[0] != '\0') where += std::string(".") + call.hook;
    if (call.node != nullptr) where += " on '" + call.node->name + "'";
    return where;
}

namespace detail {

CurrentScriptCall::CurrentScriptCall(const ScriptCall& call) : previous_{g_current} { g_current = &call; }

CurrentScriptCall::~CurrentScriptCall() { g_current = previous_; }

void report_script_exception(const ScriptCall& call, const std::exception* error) {
    const std::string where = describe(call);
    if (error != nullptr) {
        FJELL_CORE_ERROR("Exception in {}: {}", where, error->what());
    } else {
        FJELL_CORE_ERROR("Unknown exception in {}", where);
    }
}

} // namespace detail

} // namespace fjell
