#pragma once

#include "scene/node_component.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>

namespace fjell {

struct VfxEmitter;

enum class ParticleBlendMode : uint8_t {
    additive,   // fire, sparks, magic
    alpha,      // smoke, dust
};

struct EmitterComponent : NodeComponent {
    // ── Legacy flat fields (used for scene serialization compat) ──
    float emission_rate{50.0f};
    float lifetime_min{1.0f};
    float lifetime_max{3.0f};
    uint32_t max_particles{10000};
    glm::vec3 velocity_min{-0.5f, 1.0f, -0.5f};
    glm::vec3 velocity_max{0.5f, 3.0f, 0.5f};
    glm::vec3 gravity{0.0f, -9.81f, 0.0f};
    glm::vec4 color_start{1.0f, 0.8f, 0.2f, 1.0f};
    glm::vec4 color_end{1.0f, 0.1f, 0.0f, 0.0f};
    float size_start{0.1f};
    float size_end{0.02f};
    ParticleBlendMode blend_mode{ParticleBlendMode::additive};

    // ── Module-based emitter (the actual runtime config) ──
    std::shared_ptr<VfxEmitter> vfx;

    /// Ensure the module stack is built (call before GPU upload)
    void ensure_vfx();

    // Runtime (not serialized)
    float emit_accumulator{0.0f};
};

} // namespace fjell
