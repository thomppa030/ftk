#pragma once

// A rectangle in layout units, from its top-left corner to its
// bottom-right one.

#include <glm/common.hpp>
#include <glm/vec2.hpp>

namespace ftk::ui::core {

struct Rect {
    glm::vec2 min{0.0f};
    glm::vec2 max{0.0f};

    [[nodiscard]] glm::vec2 size() const { return max - min; }
    [[nodiscard]] float width() const { return max.x - min.x; }
    [[nodiscard]] float height() const { return max.y - min.y; }
    [[nodiscard]] glm::vec2 centre() const { return 0.5f * (min + max); }
    [[nodiscard]] bool empty() const { return max.x <= min.x || max.y <= min.y; }
    /// `point` is inside, its right and bottom edges excluded.
    [[nodiscard]] bool contains(glm::vec2 point) const {
        return point.x >= min.x && point.y >= min.y && point.x < max.x && point.y < max.y;
    }
    /// What this and `other` share; empty when they don't meet.
    [[nodiscard]] Rect intersect(const Rect& other) const {
        const Rect shared{.min = glm::max(min, other.min), .max = glm::min(max, other.max)};
        return {.min = shared.min, .max = glm::max(shared.max, shared.min)};
    }
    /// Both, and what lies between them.
    [[nodiscard]] Rect merge(const Rect& other) const {
        return {.min = glm::min(min, other.min), .max = glm::max(max, other.max)};
    }
    bool operator==(const Rect&) const = default;
};

} // namespace ftk::ui::core
