#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fjell {

// A point in a 1D blend space: an animation clip at a parameter position.
struct BlendSpacePoint {
    std::string clip;   // path to .fjanim
    float position;     // parameter value (e.g. 0 = idle, 0.5 = walk, 1.0 = run)
};

// Result of evaluating a BlendSpace1D: the two neighboring clips and their weights.
struct BlendResult {
    uint32_t index_a{0};
    uint32_t index_b{0};
    float weight_a{1.0f};  // weight for clip A
    float weight_b{0.0f};  // weight for clip B
};

// 1D blend space. Sorted list of clip-position points. Given a parameter value,
// finds the two neighboring points and computes blend weights via linear interpolation.
class BlendSpace1D {
public:
    void add(BlendSpacePoint point);
    void clear() { points_.clear(); }

    // Evaluate the blend space at the given parameter value.
    // Returns indices into points() and interpolation weights.
    [[nodiscard]] BlendResult evaluate(float param) const;

    [[nodiscard]] const std::vector<BlendSpacePoint>& points() const { return points_; }
    [[nodiscard]] bool empty() const { return points_.empty(); }
    [[nodiscard]] uint32_t size() const { return static_cast<uint32_t>(points_.size()); }

private:
    std::vector<BlendSpacePoint> points_; // sorted by position
};

} // namespace fjell
