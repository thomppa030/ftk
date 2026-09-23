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
    bool looping{true};   // re-fire bursts after all particles expire
    bool fire{false};     // one-frame flag: trigger burst manually
};

class Project;
class ResourceManager;

/// Loads the effect vfx_path names, with the textures and materials its
/// modules use, as this emitter's own copy. Empty or unreadable leaves no
/// effect, which draws nothing.
void load_emitter_effect(EmitterComponent& emitter, ResourceManager& resources, const Project* project);

} // namespace fjell
