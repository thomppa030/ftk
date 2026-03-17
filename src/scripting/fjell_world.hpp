// fjell_world.hpp — Engine services accessible from game code
// Shipped to game projects alongside fjell_api.h.

#pragma once

#include "core/input.hpp"

namespace fjell {

class PhysicsWorld;
class Scene;

struct World {
    const Input* input_{nullptr};
    PhysicsWorld* physics_{nullptr};
    Scene* scene_{nullptr};
    float delta_time{0.0f};
    float elapsed_time{0.0f};

    [[nodiscard]] const Input& input() const { return *input_; }
    [[nodiscard]] PhysicsWorld* physics() { return physics_; }
    [[nodiscard]] Scene* scene() { return scene_; }
};

} // namespace fjell
