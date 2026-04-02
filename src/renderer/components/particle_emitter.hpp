#pragma once

#include "scene/node_component.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace fjell {

struct VfxEffect;

enum class ParticleBlendMode : uint8_t {
    additive,   // fire, sparks, magic
    alpha,      // smoke, dust
};

struct EmitterComponent : NodeComponent {
    std::shared_ptr<VfxEffect> effect;
    std::string vfx_path; // .fjvfx asset path
};

} // namespace fjell
