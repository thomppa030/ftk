#include "scripting/script_globals.hpp"

namespace fjell {

static ScriptServices s_services;

ScriptServices& script_services() { return s_services; }

} // namespace fjell
