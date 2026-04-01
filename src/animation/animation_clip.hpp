#pragma once

#include "animation/anim_notify.hpp"
#include "animation/fjanim.hpp"
#include "core/result.hpp"

#include <ozz/animation/runtime/animation.h>

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <vector>

namespace fjell {

// A single notify placement on a clip timeline.
struct AnimNotifyInstance {
    float time{0.0f};
    float end_time{0.0f};          // >0 for state notifies (span)
    std::string type_name;
    std::unique_ptr<AnimNotify> instance;
    nlohmann::json properties;     // serialized fallback when type not loaded
};

class AnimationClip {
public:
    [[nodiscard]] static Result<AnimationClip> load(const std::string& path);
    [[nodiscard]] Result<> save(const std::string& path) const;

    [[nodiscard]] const ozz::animation::Animation& raw() const { return animation_; }
    [[nodiscard]] float duration() const { return animation_.duration(); }
    [[nodiscard]] const std::string& name() const { return name_; }

    [[nodiscard]] const std::vector<AnimJointTrack>& tracks() const { return tracks_; }
    [[nodiscard]] const std::vector<AnimNotifyInstance>& notifies() const { return notifies_; }
    [[nodiscard]] std::vector<AnimNotifyInstance>& notifies() { return notifies_; }

private:
    ozz::animation::Animation animation_;
    std::string name_;
    std::vector<AnimJointTrack> tracks_;
    std::vector<AnimNotifyInstance> notifies_;
};

} // namespace fjell
