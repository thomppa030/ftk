#pragma once

#include "animation/animation_state_machine.hpp"
#include "renderer/sprite_sheet.hpp"
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

} // namespace fjell
