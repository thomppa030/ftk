#pragma once

#include "ftk/base/delegate.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

namespace fjell {

enum class ActionType : uint8_t {
    button,   // bool: pressed/held/released
    axis1d,   // float: -1..1
    axis2d,   // vec2: -1..1 per axis
};

struct InputActionDef {
    std::string name;
    ActionType type{ActionType::button};
};

struct ActionState {
    ActionType type{ActionType::button};
    bool pressed{};       // just pressed this frame
    bool held{};          // currently held
    bool released{};      // just released this frame
    bool was_active{};    // active last frame (for triggered/completed detection)
    float value{};        // axis1d value
    glm::vec2 axis{};     // axis2d value
};

struct ActionDelegates {
    // Button-specific
    ftk::Delegate<void()> on_pressed;      // button: just went down
    ftk::Delegate<void()> on_released;     // button: just went up

    // Shared lifecycle
    ftk::Delegate<void()> on_triggered;    // any type: action became active this frame
    ftk::Delegate<void()> on_completed;    // any type: action became inactive this frame

    // Continuous value (fires every frame while active)
    ftk::Delegate<void(float)> on_axis1d;
    ftk::Delegate<void(glm::vec2)> on_axis2d;
};

} // namespace fjell
