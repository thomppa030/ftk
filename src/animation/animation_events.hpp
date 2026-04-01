#pragma once

#include "core/delegate.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fjell {

struct AnimationEvent {
    float time{0.0f};
    std::string name;
    std::unordered_map<std::string, std::string> params;
};

// Sorted event list for a single animation clip. Events are stored separately
// from the clip because ozz animations are opaque compressed blobs.
class AnimationEventTrack {
public:
    void add(AnimationEvent event);

    // Fire all events in the time range (prev_time, current_time].
    // Handles forward playback and loop wraparound (when current < prev).
    void fire_in_range(float prev_time, float current_time, float duration);

    // Same as above, but also calls an external callback for each fired event.
    void fire_in_range(float prev_time, float current_time, float duration,
                       const std::function<void(const AnimationEvent&)>& callback);

    void clear() { events_.clear(); }
    [[nodiscard]] bool empty() const { return events_.empty(); }
    [[nodiscard]] uint32_t size() const { return static_cast<uint32_t>(events_.size()); }
    [[nodiscard]] const std::vector<AnimationEvent>& events() const { return events_; }

    Delegate<void(const AnimationEvent&)> on_event;

private:
    std::vector<AnimationEvent> events_; // sorted by time
};

} // namespace fjell
