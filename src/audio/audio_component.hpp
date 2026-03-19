#pragma once

#include "scene/node_component.hpp"

#include <cstdint>
#include <string>

namespace fjell {

struct AudioSourceComponent : NodeComponent {
    std::string clip_path;
    float volume{1.0f};
    float pitch{1.0f};
    float min_distance{1.0f};
    float max_distance{50.0f};
    bool loop{false};
    bool spatial{true};
    bool play_on_start{false};

    // Runtime state (not serialized)
    bool playing{false};
    uint32_t sound_id{0};
    bool sound_loaded{false};
};

} // namespace fjell
