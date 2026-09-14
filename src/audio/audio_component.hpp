#pragma once

#include "scene/component_storage.hpp"
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

// A duplicate takes the authored settings only. The sound handle belongs to
// the source's playback; a copy claiming it would stop or restart a sound it
// never started.
inline void clone_component(const ComponentStorage& /*from*/, const AudioSourceComponent& src,
                            ComponentStorage& to) {
    auto& copy = to.add<AudioSourceComponent>();
    copy.clip_path = src.clip_path;
    copy.volume = src.volume;
    copy.pitch = src.pitch;
    copy.min_distance = src.min_distance;
    copy.max_distance = src.max_distance;
    copy.loop = src.loop;
    copy.spatial = src.spatial;
    copy.play_on_start = src.play_on_start;
}

} // namespace fjell
