// fixed_step.hpp — What a fixed step of the simulation looks like to a script
#pragma once

#include "fjell/api.h"
#include "fjell/input_action_ref.hpp"
#include "fjell/ref.hpp"

#include <glm/glm.hpp>

#include <cstdint>

extern "C" {
FJELL_SDK_EXPORT int   fjell_step_input_pressed(uint32_t action_handle_id);
FJELL_SDK_EXPORT int   fjell_step_input_released(uint32_t action_handle_id);
FJELL_SDK_EXPORT int   fjell_step_input_held(uint32_t action_handle_id);
FJELL_SDK_EXPORT float fjell_step_input_axis1d(uint32_t action_handle_id);
FJELL_SDK_EXPORT void  fjell_step_input_axis2d(uint32_t action_handle_id, float* x, float* y);
}

namespace fjell {

/// The input a fixed step sees. Frames and steps do not line up, so this is
/// not the frame's input: a press is seen by exactly one step, the first to
/// begin after it, even when the frame it happened on ran no step or several.
/// Held buttons and axes read as the latest frame left them.
///
/// Only meaningful inside `fixed_tick()`, the one place a `FixedStep` is
/// handed out; read elsewhere, the engine reports it and answers as if
/// nothing were pressed.
///
/// @code
/// void fixed_tick(const fjell::FixedStep& step) override {
///     if (step.input.pressed(boost) && boost_ready_) { /* ... */ }
///     const glm::vec2 stick = step.input.axis2d(move);
/// }
/// @endcode
struct StepInput {
    /// Whether the action was pressed since the step before this one.
    [[nodiscard]] bool pressed(const Ref<InputAction>& action) const {
        return fjell_step_input_pressed(action.handle_id()) != 0;
    }

    /// Whether the action was released since the step before this one.
    [[nodiscard]] bool released(const Ref<InputAction>& action) const {
        return fjell_step_input_released(action.handle_id()) != 0;
    }

    /// Whether the action is held.
    [[nodiscard]] bool held(const Ref<InputAction>& action) const {
        return fjell_step_input_held(action.handle_id()) != 0;
    }

    /// A one-axis action's value.
    [[nodiscard]] float axis1d(const Ref<InputAction>& action) const {
        return fjell_step_input_axis1d(action.handle_id());
    }

    /// A two-axis action's value, such as a stick or WASD.
    [[nodiscard]] glm::vec2 axis2d(const Ref<InputAction>& action) const {
        glm::vec2 value{0.0f};
        fjell_step_input_axis2d(action.handle_id(), &value.x, &value.y);
        return value;
    }
};

/// One step of the simulation, as `ScriptComponent::fixed_tick()` receives it.
/// Every step of a session has the same length, which is what makes the
/// simulation behave the same at any frame rate and replay exactly.
struct FixedStep {
    /// The step's length in seconds, the same for every step.
    float dt{0.0f};
    /// Simulation time when this step starts, in seconds.
    double time{0.0};
    /// How many steps the session took before this one.
    uint64_t index{0};
    /// The input for this step.
    StepInput input{};
};

} // namespace fjell
