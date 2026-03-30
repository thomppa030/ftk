#pragma once

#include "renderer/sprite_sheet.hpp"
#include "scene/node_component.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace fjell {

struct SpriteAnimComponent : NodeComponent {
    // Serialized
    std::unordered_map<std::string, std::string> animations; // name -> .fjsheet path
    std::string default_animation;
    float playback_speed{1.0f};

    // Runtime (not serialized)
    std::string current_animation;
    uint32_t current_frame{0};
    float frame_timer{0.0f};
    bool playing{true};
    bool finished{false};

    // Loaded sheets (keyed by animation name)
    std::unordered_map<std::string, SpriteSheet> sheets;
    bool sheets_loaded{false};
};

} // namespace fjell
