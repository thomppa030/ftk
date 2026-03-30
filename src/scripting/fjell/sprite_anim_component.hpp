// sprite_anim_component.hpp — Sprite animation component for game scripts.
// Layout-matching copy of renderer/sprite_anim_component.hpp, shipped as part of the SDK.
#pragma once

#include "fjell/animation_state_machine.hpp"
#include "fjell/sprite_sheet.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace fjell {

struct SpriteAnimComponent {
    static constexpr const char* type_name = "SpriteAnimComponent";

    // Serialized
    std::string animset_path;
    float playback_speed{1.0f};

    // Legacy
    std::unordered_map<std::string, std::string> animations;
    std::string default_animation;

    // Runtime
    AnimationStateMachine state_machine;
    std::unordered_map<std::string, SpriteSheet> sheets;
    uint32_t current_frame{0};
    float frame_timer{0.0f};
    bool playing{true};
    bool finished{false};
    bool loaded{false};
};

} // namespace fjell
