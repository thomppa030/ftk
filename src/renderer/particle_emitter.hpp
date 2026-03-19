#pragma once

#include "scene/node_component.hpp"

#include <glm/glm.hpp>

#include <cstdint>

namespace fjell {

enum class ParticleBlendMode : uint8_t {
    additive,   // fire, sparks, magic
    alpha,      // smoke, dust
};

struct EmitterComponent : NodeComponent {
    // Emission
    float emission_rate{50.0f};       // particles per second
    float lifetime_min{1.0f};
    float lifetime_max{3.0f};
    uint32_t max_particles{10000};

    // Initial velocity
    glm::vec3 velocity_min{-0.5f, 1.0f, -0.5f};
    glm::vec3 velocity_max{0.5f, 3.0f, 0.5f};

    // Physics
    glm::vec3 gravity{0.0f, -9.81f, 0.0f};

    // Appearance
    glm::vec4 color_start{1.0f, 0.8f, 0.2f, 1.0f};
    glm::vec4 color_end{1.0f, 0.1f, 0.0f, 0.0f};
    float size_start{0.1f};
    float size_end{0.02f};

    // Blend mode
    ParticleBlendMode blend_mode{ParticleBlendMode::additive};

    // Runtime (not serialized)
    float emit_accumulator{0.0f};
};

} // namespace fjell
