// fjell_world.hpp — Engine services accessible from game code
// Shipped to game projects alongside fjell_api.h.

#pragma once

#include "fjell_input.hpp"

namespace fjell {

class PhysicsWorld;
class Scene;

struct World {
    const Input* input_{nullptr};
    PhysicsWorld* physics_{nullptr};
    Scene* scene_{nullptr};
    float delta_time{0.0f};
    float elapsed_time{0.0f};

    // Logging (engine sets these function pointers)
    void (*log_info_fn)(const char*){nullptr};
    void (*log_warn_fn)(const char*){nullptr};
    void (*log_error_fn)(const char*){nullptr};

    [[nodiscard]] const Input& input() const { return *input_; }
    [[nodiscard]] PhysicsWorld* physics() { return physics_; }
    [[nodiscard]] Scene* scene() { return scene_; }

    void log_info(const char* msg) const { if (log_info_fn) log_info_fn(msg); }
    void log_warn(const char* msg) const { if (log_warn_fn) log_warn_fn(msg); }
    void log_error(const char* msg) const { if (log_error_fn) log_error_fn(msg); }
};

} // namespace fjell
