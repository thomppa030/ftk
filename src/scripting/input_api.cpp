#include "core/input_action_map.hpp"
#include "core/delegate.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "scripting/fjell/api.h"
#include "scripting/script_globals.hpp"

#include <vector>

namespace {

std::vector<fjell::Connection> s_input_connections;

fjell::InputActionMap* get_iam() {
    return fjell::InputActionMap::instance();
}

const char* resolve_action_name(uint32_t handle_id) {
    auto* rm = fjell::script_resource_manager();
    if (!rm) return nullptr;
    auto* def = rm->get(fjell::InputActionHandle{handle_id});
    return def ? def->name.c_str() : nullptr;
}

} // anonymous namespace

extern "C" void fjell_input_bind_void(uint32_t action_handle_id, int trigger,
                                       void* instance, void (*fn)(void*)) {
    auto* iam = get_iam();
    if (!iam) return;
    const char* name = resolve_action_name(action_handle_id);
    if (!name) return;
    auto* d = iam->delegates_for(name);
    if (!d) return;

    void* inst = instance;
    switch (static_cast<fjell::EInputTrigger>(trigger)) {
        case fjell::EInputTrigger::Triggered:
            s_input_connections.push_back(d->on_triggered.bind([fn, inst]() { fn(inst); }));
            break;
        case fjell::EInputTrigger::Pressed:
            s_input_connections.push_back(d->on_pressed.bind([fn, inst]() { fn(inst); }));
            break;
        case fjell::EInputTrigger::Released:
            s_input_connections.push_back(d->on_released.bind([fn, inst]() { fn(inst); }));
            break;
        case fjell::EInputTrigger::Completed:
            s_input_connections.push_back(d->on_completed.bind([fn, inst]() { fn(inst); }));
            break;
    }
}

extern "C" void fjell_input_bind_float(uint32_t action_handle_id, int trigger,
                                        void* instance, void (*fn)(void*, float)) {
    auto* iam = get_iam();
    if (!iam) return;
    const char* name = resolve_action_name(action_handle_id);
    if (!name) return;
    auto* d = iam->delegates_for(name);
    if (!d) return;

    void* inst = instance;
    // axis1d fires continuously while active
    s_input_connections.push_back(d->on_axis1d.bind([fn, inst](float v) { fn(inst, v); }));

    // Also connect triggered/completed if requested
    if (static_cast<fjell::EInputTrigger>(trigger) == fjell::EInputTrigger::Completed) {
        s_input_connections.push_back(d->on_completed.bind([fn, inst]() { fn(inst, 0.0f); }));
    }
}

extern "C" void fjell_input_bind_vec2(uint32_t action_handle_id, int trigger,
                                       void* instance, void (*fn)(void*, float, float)) {
    auto* iam = get_iam();
    if (!iam) return;
    const char* name = resolve_action_name(action_handle_id);
    if (!name) return;
    auto* d = iam->delegates_for(name);
    if (!d) return;

    void* inst = instance;
    // axis2d fires continuously while active
    s_input_connections.push_back(
        d->on_axis2d.bind([fn, inst](glm::vec2 v) { fn(inst, v.x, v.y); }));

    // Connect completed if requested (sends zero)
    if (static_cast<fjell::EInputTrigger>(trigger) == fjell::EInputTrigger::Completed) {
        s_input_connections.push_back(
            d->on_completed.bind([fn, inst]() { fn(inst, 0.0f, 0.0f); }));
    }
}

extern "C" void fjell_input_clear_connections() {
    s_input_connections.clear();
}

extern "C" void fjell_input_push_context(const char* name) {
    auto* iam = get_iam();
    if (iam && name) iam->push_context(name);
}

extern "C" void fjell_input_pop_context(const char* name) {
    auto* iam = get_iam();
    if (iam && name) iam->pop_context(name);
}
