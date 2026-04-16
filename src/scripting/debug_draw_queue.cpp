// debug_draw_queue.cpp — Script-submitted debug draw primitives
#include "scripting/debug_draw_queue.hpp"

namespace fjell {

void DebugDrawQueue::line(const glm::vec3& from, const glm::vec3& to,
                          const glm::vec4& color, float ttl) {
    primitives_.push_back({DebugPrimitive::Type::Line, from, to, 0.0f, color, ttl});
}

void DebugDrawQueue::box(const glm::vec3& center, const glm::vec3& half_extents,
                         const glm::vec4& color, float ttl) {
    primitives_.push_back({DebugPrimitive::Type::Box, center, half_extents, 0.0f, color, ttl});
}

void DebugDrawQueue::sphere(const glm::vec3& center, float radius,
                            const glm::vec4& color, float ttl) {
    primitives_.push_back({DebugPrimitive::Type::Sphere, center, {}, radius, color, ttl});
}

void DebugDrawQueue::point(const glm::vec3& pos,
                           const glm::vec4& color, float ttl) {
    primitives_.push_back({DebugPrimitive::Type::Point, pos, {}, 0.0f, color, ttl});
}

void DebugDrawQueue::tick(float dt) {
    for (auto it = primitives_.begin(); it != primitives_.end();) {
        if (it->ttl <= 0.0f) {
            // Single-frame primitive — remove after this frame
            it = primitives_.erase(it);
        } else {
            it->ttl -= dt;
            ++it;
        }
    }
}

void DebugDrawQueue::clear() {
    primitives_.clear();
}

} // namespace fjell
