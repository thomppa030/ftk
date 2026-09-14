#pragma once

#include "animation/animation_state_machine.hpp"
#include "renderer/systems/sprite_sheet.hpp"
#include "scene/component_storage.hpp"
#include "scene/node_component.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace fjell {

struct SpriteAnimComponent : NodeComponent {
    static constexpr const char* type_name = "SpriteAnimComponent";

    // Serialized
    std::string animset_path;       // .fjanimset file
    float playback_speed{1.0f};

    // Legacy (backward compat): direct animation map
    std::unordered_map<std::string, std::string> animations; // name -> .fjsheet path
    std::string default_animation;

    // Runtime (not serialized)
    AnimationStateMachine state_machine;
    std::unordered_map<std::string, SpriteSheet> sheets; // state name -> loaded sheet
    uint32_t current_frame{0};
    float frame_timer{0.0f};
    bool playing{true};
    bool finished{false};
    bool loaded{false};
};

// A duplicate takes the authored animation set and loads its own sheets; the
// playback state starts from the beginning rather than mid-animation.
inline void clone_component(const ComponentStorage& /*from*/, const SpriteAnimComponent& src,
                            ComponentStorage& to) {
    auto& copy = to.add<SpriteAnimComponent>();
    copy.animset_path = src.animset_path;
    copy.animations = src.animations;
    copy.default_animation = src.default_animation;
    copy.playback_speed = src.playback_speed;
}

} // namespace fjell
