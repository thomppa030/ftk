// world.hpp — Per-frame runtime state for game scripts
#pragma once

#include "fjell/input.hpp"

namespace fjell {

class PhysicsWorld;

struct World {
    const Input* input_{nullptr};
    PhysicsWorld* physics_{nullptr};
    float delta_time{0.0f};
    float elapsed_time{0.0f};

    [[nodiscard]] const Input& input() const { return *input_; }
    [[nodiscard]] PhysicsWorld* physics() { return physics_; }
};

} // namespace fjell
