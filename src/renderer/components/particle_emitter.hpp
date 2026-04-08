#pragma once

#include "scene/node_component.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace fjell {

struct VfxEffect;

enum class ParticleBlendMode : uint8_t {
    additive,   // fire, sparks, magic — transparent pass, order-independent
    alpha,      // smoke, dust — transparent pass, sorted back-to-front
    opaque,     // mesh debris, solid particles — opaque pass, depth writes
};

struct EmitterComponent : NodeComponent {
    std::shared_ptr<VfxEffect> effect;
    std::string vfx_path; // .fjvfx asset path
};

} // namespace fjell
