// debug_draw_queue.hpp — Script-submitted debug draw primitives
// CPU-side queue consumed by DebugDraw each frame. No Vulkan dependency.
#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace fjell {

struct DebugPrimitive {
    enum class Type : uint8_t { Line, Box, Sphere, Point };

    Type type;
    glm::vec3 a{0.0f};       // line: start, box: center, sphere: center, point: pos
    glm::vec3 b{0.0f};       // line: end, box: half_extents, sphere: unused
    float radius{0.0f};       // sphere radius, point size (unused for line/box)
    glm::vec4 color{1.0f};
    float ttl{0.0f};          // time-to-live in seconds (0 = single frame)
};

class DebugDrawQueue {
public:
    void line(const glm::vec3& from, const glm::vec3& to,
              const glm::vec4& color, float ttl = 0.0f);

    void box(const glm::vec3& center, const glm::vec3& half_extents,
             const glm::vec4& color, float ttl = 0.0f);

    void sphere(const glm::vec3& center, float radius,
                const glm::vec4& color, float ttl = 0.0f);

    void point(const glm::vec3& pos,
               const glm::vec4& color, float ttl = 0.0f);

    /// Drain all primitives. Called once per frame by DebugDraw.
    /// Decrements TTL on persistent primitives, removes expired ones.
    void tick(float dt);

    [[nodiscard]] const std::vector<DebugPrimitive>& primitives() const { return primitives_; }

    void clear();

private:
    std::vector<DebugPrimitive> primitives_;
};

} // namespace fjell
