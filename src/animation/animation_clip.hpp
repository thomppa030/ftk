#pragma once

#include <ozz/animation/runtime/animation.h>

#include <string>

namespace fjell {

class AnimationClip {
public:
    [[nodiscard]] static AnimationClip load(const std::string& path);

    [[nodiscard]] const ozz::animation::Animation& raw() const { return animation_; }
    [[nodiscard]] float duration() const { return animation_.duration(); }
    [[nodiscard]] const std::string& name() const { return name_; }

private:
    ozz::animation::Animation animation_;
    std::string name_;
};

} // namespace fjell
