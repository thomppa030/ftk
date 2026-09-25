#include "core/input.hpp"
#include "core/input_action_map.hpp"
#include "core/log.hpp"
#include "core/step_input_latch.hpp"
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

// ── Step input ──────────────────────────────────────────────────────────

namespace {

/// The action's name, if it is read where step input means something: inside
/// a fixed step. Anywhere else the read is reported once and answered as if
/// nothing were pressed, rather than with whatever the last step saw.
const char* step_action(uint32_t action_handle_id) {
    auto& services = fjell::script_services();
    if (!services.in_fixed_step) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            FJELL_CORE_WARN("Step input read outside fixed_tick(): it is the input of one fixed "
                            "step, and outside one there is none. Bind the action with "
                            "InputAPI::bind() to react to it every frame instead");
        }
        return nullptr;
    }
    return resolve_action_name(action_handle_id);
}

} // namespace

extern "C" FJELL_SDK_EXPORT int fjell_step_input_pressed(uint32_t action_handle_id) {
    const char* name = step_action(action_handle_id);
    const auto* input = fjell::script_services().step_input;
    return name != nullptr && input != nullptr && input->pressed(name) ? 1 : 0;
}

extern "C" FJELL_SDK_EXPORT int fjell_step_input_released(uint32_t action_handle_id) {
    const char* name = step_action(action_handle_id);
    const auto* input = fjell::script_services().step_input;
    return name != nullptr && input != nullptr && input->released(name) ? 1 : 0;
}

extern "C" FJELL_SDK_EXPORT int fjell_step_input_held(uint32_t action_handle_id) {
    const char* name = step_action(action_handle_id);
    auto* iam = get_iam();
    return name != nullptr && iam != nullptr && iam->is_held(name) ? 1 : 0;
}

extern "C" FJELL_SDK_EXPORT float fjell_step_input_axis1d(uint32_t action_handle_id) {
    const char* name = step_action(action_handle_id);
    auto* iam = get_iam();
    return name != nullptr && iam != nullptr ? iam->axis1d(name) : 0.0f;
}

extern "C" FJELL_SDK_EXPORT void fjell_step_input_axis2d(uint32_t action_handle_id, float* x,
                                                         float* y) {
    const char* name = step_action(action_handle_id);
    auto* iam = get_iam();
    const glm::vec2 value = name != nullptr && iam != nullptr ? iam->axis2d(name) : glm::vec2{0.0f};
    *x = value.x;
    *y = value.y;
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
