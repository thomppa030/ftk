#include "core/input.hpp"
#include "core/input_action_map.hpp"
#include "core/delegate.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "scripting/fjell/api.h"
#include "scripting/input_bindings.hpp"
#include "scripting/script_globals.hpp"

#include <vector>

namespace {

// Every binding a script made, with the instance it calls into, so one that
// goes away mid-play takes its own bindings with it.
struct OwnedConnection {
    const void* owner;
    fjell::Connection connection;
};
std::vector<OwnedConnection> s_input_connections;

fjell::InputActionMap* get_iam() {
    return fjell::script_services().input_actions;
}

const char* resolve_action_name(uint32_t handle_id) {
    auto* rm = fjell::script_services().resources;
    if (!rm) return nullptr;
    auto* def = rm->get(fjell::InputActionHandle{handle_id});
    return def ? def->name.c_str() : nullptr;
}

} // anonymous namespace

extern "C" FJELL_SDK_EXPORT void fjell_input_bind_void(uint32_t action_handle_id, int trigger,
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
            s_input_connections.push_back({inst, d->on_triggered.bind([fn, inst]() { fn(inst); })});
            break;
        case fjell::EInputTrigger::Pressed:
            s_input_connections.push_back({inst, d->on_pressed.bind([fn, inst]() { fn(inst); })});
            break;
        case fjell::EInputTrigger::Released:
            s_input_connections.push_back({inst, d->on_released.bind([fn, inst]() { fn(inst); })});
            break;
        case fjell::EInputTrigger::Completed:
            s_input_connections.push_back({inst, d->on_completed.bind([fn, inst]() { fn(inst); })});
            break;
    }
}

extern "C" FJELL_SDK_EXPORT void fjell_input_bind_float(uint32_t action_handle_id, int trigger,
                                        void* instance, void (*fn)(void*, float)) {
    auto* iam = get_iam();
    if (!iam) return;
    const char* name = resolve_action_name(action_handle_id);
    if (!name) return;
    auto* d = iam->delegates_for(name);
    if (!d) return;

    void* inst = instance;
    // axis1d fires continuously while active
    s_input_connections.push_back({inst, d->on_axis1d.bind([fn, inst](float v) { fn(inst, v); })});

    // Also connect triggered/completed if requested
    if (static_cast<fjell::EInputTrigger>(trigger) == fjell::EInputTrigger::Completed) {
        s_input_connections.push_back({inst, d->on_completed.bind([fn, inst]() { fn(inst, 0.0f); })});
    }
}

extern "C" FJELL_SDK_EXPORT void fjell_input_bind_vec2(uint32_t action_handle_id, int trigger,
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
        OwnedConnection{inst, d->on_axis2d.bind([fn, inst](glm::vec2 v) { fn(inst, v.x, v.y); })});

    // Connect completed if requested (sends zero)
    if (static_cast<fjell::EInputTrigger>(trigger) == fjell::EInputTrigger::Completed) {
        s_input_connections.push_back(
            OwnedConnection{inst, d->on_completed.bind([fn, inst]() { fn(inst, 0.0f, 0.0f); })});
    }
}

extern "C" FJELL_SDK_EXPORT void fjell_input_clear_connections() {
    s_input_connections.clear();
}

namespace fjell {

void release_input_bindings(const void* owner) {
    if (owner == nullptr) return;
    std::erase_if(s_input_connections,
                  [owner](const OwnedConnection& c) { return c.owner == owner; });
}

} // namespace fjell

extern "C" FJELL_SDK_EXPORT void fjell_input_push_context(const char* name) {
    auto* iam = get_iam();
    if (iam && name) iam->push_context(name);
}

extern "C" FJELL_SDK_EXPORT void fjell_input_pop_context(const char* name) {
    auto* iam = get_iam();
    if (iam && name) iam->pop_context(name);
}

// EInputDevice is a hand-mirrored copy of InputDevice, and the value crosses
// the boundary as a plain int. This is the only place that sees both, so it
// is where the two can be held together.
static_assert(static_cast<int>(fjell::EInputDevice::KeyboardMouse)
                  == static_cast<int>(fjell::InputDevice::keyboard_mouse),
              "EInputDevice must mirror InputDevice");
static_assert(static_cast<int>(fjell::EInputDevice::Gamepad)
                  == static_cast<int>(fjell::InputDevice::gamepad),
              "EInputDevice must mirror InputDevice");

extern "C" FJELL_SDK_EXPORT int fjell_input_active_device() {
    auto* input = fjell::script_services().input;
    if (!input) return static_cast<int>(fjell::InputDevice::keyboard_mouse);
    return static_cast<int>(input->active_device());
}

extern "C" FJELL_SDK_EXPORT bool fjell_input_pad_connected() {
    auto* input = fjell::script_services().input;
    return input != nullptr && input->pad_connected();
}

extern "C" FJELL_SDK_EXPORT void fjell_input_set_cursor_locked(bool locked) {
    auto* input = fjell::script_services().input;
    if (!input) return;
    // Records the request only. PlayController applies it once per frame
    // against the game's focus, so a lock asked for here survives the editor
    // taking focus away and returning without the script being involved.
    input->set_cursor_lock_wanted(locked);
}

extern "C" FJELL_SDK_EXPORT bool fjell_input_cursor_locked() {
    auto* input = fjell::script_services().input;
    return input != nullptr && input->is_cursor_captured();
}
