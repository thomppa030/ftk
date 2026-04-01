#include "animation/animation_events.hpp"

#include <algorithm>

namespace fjell {

void AnimationEventTrack::add(AnimationEvent event) {
    float t = event.time;
    auto it = std::ranges::lower_bound(events_, t, {}, &AnimationEvent::time);
    events_.insert(it, std::move(event));
}

void AnimationEventTrack::fire_in_range(float prev_time, float current_time, float duration) {
    fire_in_range(prev_time, current_time, duration, nullptr);
}

void AnimationEventTrack::fire_in_range(float prev_time, float current_time, float duration,
                                         const std::function<void(const AnimationEvent&)>& callback) {
    if (events_.empty() || duration <= 0.0f) return;

    if (current_time > prev_time) {
        // Normal forward playback: fire events in (prev_time, current_time]
        for (const auto& e : events_) {
            if (e.time > prev_time && e.time <= current_time) {
                on_event.broadcast(e);
                if (callback) callback(e);
            }
        }
    } else {
        // Loop wraparound: fire (prev_time, duration] then [0, current_time]
        for (const auto& e : events_) {
            if (e.time > prev_time && e.time <= duration) {
                on_event.broadcast(e);
                if (callback) callback(e);
            }
        }
        for (const auto& e : events_) {
            if (e.time > 0.0f && e.time <= current_time) {
                on_event.broadcast(e);
                if (callback) callback(e);
            }
        }
    }
}

} // namespace fjell
