#include "animation/blend_space.hpp"

#include <algorithm>

namespace fjell {

void BlendSpace1D::add(BlendSpacePoint point) {
    float pos = point.position;
    auto it = std::ranges::lower_bound(points_, pos, {}, &BlendSpacePoint::position);
    points_.insert(it, std::move(point));
}

BlendResult BlendSpace1D::evaluate(float param) const {
    if (points_.empty()) return {};

    if (points_.size() == 1) {
        return {0, 0, 1.0f, 0.0f};
    }

    // Clamp to range
    if (param <= points_.front().position) {
        return {0, 0, 1.0f, 0.0f};
    }
    if (param >= points_.back().position) {
        auto last = static_cast<uint32_t>(points_.size() - 1);
        return {last, last, 1.0f, 0.0f};
    }

    // Find the two neighboring points
    for (uint32_t i = 0; i < static_cast<uint32_t>(points_.size()) - 1; ++i) {
        if (param >= points_[i].position && param <= points_[i + 1].position) {
            float range = points_[i + 1].position - points_[i].position;
            float t = (range > 0.0f) ? (param - points_[i].position) / range : 0.0f;
            return {i, i + 1, 1.0f - t, t};
        }
    }

    // Fallback (shouldn't reach here)
    return {0, 0, 1.0f, 0.0f};
}

} // namespace fjell
