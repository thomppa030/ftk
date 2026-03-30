#include "scripting/script_globals.hpp"
#include "core/input.hpp"

extern "C" void fjell_input_set_cursor_captured(bool captured) {
    auto* input = fjell::script_input();
    if (!input || !input->window()) return;
    input->set_cursor_captured(input->window(), captured);
}
