#pragma once

#include "animation/animation_events.hpp"
#include "animation/fjanim.hpp"
#include "core/result.hpp"

#include <ozz/animation/runtime/animation.h>

#include <string>
#include <vector>

namespace fjell {

class AnimationClip {
public:
    [[nodiscard]] static Result<AnimationClip> load(const std::string& path);
    [[nodiscard]] Result<> save(const std::string& path) const;

    [[nodiscard]] const ozz::animation::Animation& raw() const { return animation_; }
    [[nodiscard]] float duration() const { return animation_.duration(); }
    [[nodiscard]] const std::string& name() const { return name_; }

    [[nodiscard]] const std::vector<AnimJointTrack>& tracks() const { return tracks_; }
    [[nodiscard]] const std::vector<AnimationEvent>& events() const { return events_; }
    [[nodiscard]] std::vector<AnimationEvent>& events() { return events_; }

private:
    ozz::animation::Animation animation_;
    std::string name_;
    std::vector<AnimJointTrack> tracks_;
    std::vector<AnimationEvent> events_;
};

} // namespace fjell
