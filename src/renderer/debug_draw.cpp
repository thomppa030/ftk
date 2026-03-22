#include "renderer/debug_draw.hpp"
#include "renderer/arrow_component.hpp"
#include "renderer/buffer.hpp"
#include "renderer/uniform_types.hpp"
#include "core/profiler.hpp"
#include "scene/scene.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <numbers>

namespace fjell {

DebugDraw::DebugDraw(VkDevice device, VkPhysicalDevice physical_device)
    : device_{device}, physical_device_{physical_device} {}

// ── Geometry helpers ────────────────────────────────────────────────────

void DebugDraw::add_line(const glm::vec3& a, const glm::vec3& b) {
    vertices_.push_back(a);
    vertices_.push_back(b);
}

void DebugDraw::add_circle(const glm::vec3& center, const glm::vec3& axis_a,
                           const glm::vec3& axis_b, float radius, uint32_t segments) {
    for (uint32_t i = 0; i < segments; ++i) {
        float a0 = 2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(segments);
        float a1 = 2.0f * std::numbers::pi_v<float> * static_cast<float>(i + 1) / static_cast<float>(segments);
        auto p0 = center + (axis_a * std::cos(a0) + axis_b * std::sin(a0)) * radius;
        auto p1 = center + (axis_a * std::cos(a1) + axis_b * std::sin(a1)) * radius;
        add_line(p0, p1);
    }
}

void DebugDraw::generate_box(const glm::vec3& he) {
    // 8 corners
    glm::vec3 c[8] = {
        {-he.x, -he.y, -he.z}, { he.x, -he.y, -he.z},
        { he.x,  he.y, -he.z}, {-he.x,  he.y, -he.z},
        {-he.x, -he.y,  he.z}, { he.x, -he.y,  he.z},
        { he.x,  he.y,  he.z}, {-he.x,  he.y,  he.z},
    };
    // 12 edges
    add_line(c[0], c[1]); add_line(c[1], c[2]); add_line(c[2], c[3]); add_line(c[3], c[0]);
    add_line(c[4], c[5]); add_line(c[5], c[6]); add_line(c[6], c[7]); add_line(c[7], c[4]);
    add_line(c[0], c[4]); add_line(c[1], c[5]); add_line(c[2], c[6]); add_line(c[3], c[7]);
}

void DebugDraw::generate_sphere(float radius, uint32_t segments) {
    glm::vec3 origin{0.0f};
    add_circle(origin, {1, 0, 0}, {0, 1, 0}, radius, segments);
    add_circle(origin, {1, 0, 0}, {0, 0, 1}, radius, segments);
    add_circle(origin, {0, 1, 0}, {0, 0, 1}, radius, segments);
}

void DebugDraw::generate_capsule(float radius, float height, uint32_t segments) {
    float half_h = height * 0.5f;
    glm::vec3 top{0, half_h, 0};
    glm::vec3 bot{0, -half_h, 0};

    // Two circles at hemisphere centers
    add_circle(top, {1, 0, 0}, {0, 0, 1}, radius, segments);
    add_circle(bot, {1, 0, 0}, {0, 0, 1}, radius, segments);

    // Vertical lines connecting the circles
    for (uint32_t i = 0; i < 4; ++i) {
        float angle = std::numbers::pi_v<float> * 0.5f * static_cast<float>(i);
        float x = std::cos(angle) * radius;
        float z = std::sin(angle) * radius;
        add_line({x, half_h, z}, {x, -half_h, z});
    }

    // Top and bottom semicircles (two arcs each)
    uint32_t half_seg = segments / 2;
    for (uint32_t i = 0; i < half_seg; ++i) {
        float a0 = std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(half_seg);
        float a1 = std::numbers::pi_v<float> * static_cast<float>(i + 1) / static_cast<float>(half_seg);

        // Top hemisphere arcs (XY and ZY planes)
        add_line(top + glm::vec3{std::cos(a0) * radius, std::sin(a0) * radius, 0},
                 top + glm::vec3{std::cos(a1) * radius, std::sin(a1) * radius, 0});
        add_line(top + glm::vec3{0, std::sin(a0) * radius, std::cos(a0) * radius},
                 top + glm::vec3{0, std::sin(a1) * radius, std::cos(a1) * radius});

        // Bottom hemisphere arcs (mirror Y)
        add_line(bot + glm::vec3{std::cos(a0) * radius, -std::sin(a0) * radius, 0},
                 bot + glm::vec3{std::cos(a1) * radius, -std::sin(a1) * radius, 0});
        add_line(bot + glm::vec3{0, -std::sin(a0) * radius, std::cos(a0) * radius},
                 bot + glm::vec3{0, -std::sin(a1) * radius, std::cos(a1) * radius});
    }
}

// ── Update ──────────────────────────────────────────────────────────────

void DebugDraw::update(const Scene& scene) {
    FJELL_PROFILE_SCOPE_N("debug_draw_update");
    vertices_.clear();
    draw_commands_.clear();

    for (const auto& obj : scene.objects()) {
        if (!obj->collider) continue;
        const auto& col = *obj->collider;

        uint32_t first = static_cast<uint32_t>(vertices_.size());

        switch (col.shape) {
            case ColliderShape::Box:
                generate_box(col.half_extents * obj->transform.scale);
                break;
            case ColliderShape::Sphere:
                generate_sphere(col.radius * std::max({obj->transform.scale.x,
                                                        obj->transform.scale.y,
                                                        obj->transform.scale.z}));
                break;
            case ColliderShape::Capsule:
                generate_capsule(col.radius * std::max(obj->transform.scale.x, obj->transform.scale.z),
                                 col.height * obj->transform.scale.y);
                break;
            case ColliderShape::Mesh:
                // Mesh collider wireframe not visualized (too many lines)
                break;
        }

        uint32_t count = static_cast<uint32_t>(vertices_.size()) - first;
        if (count == 0) continue;

        // Model matrix = object position + rotation + collider offset (rotated).
        // Scale is already baked into the shape dimensions, so strip it.
        glm::mat3 rot(glm::normalize(glm::vec3(obj->world_matrix_[0])),
                      glm::normalize(glm::vec3(obj->world_matrix_[1])),
                      glm::normalize(glm::vec3(obj->world_matrix_[2])));
        glm::vec3 world_offset = rot * col.offset;
        glm::mat4 model(glm::vec4(rot[0], 0.0f),
                         glm::vec4(rot[1], 0.0f),
                         glm::vec4(rot[2], 0.0f),
                         glm::vec4(glm::vec3(obj->world_matrix_[3]) + world_offset, 1.0f));
        draw_commands_.push_back({model, first, count});
    }

    // Draw arrows for nodes with ArrowComponent
    for (const auto& obj : scene.objects()) {
        const auto* arrow_ptr = obj->components.get<ArrowComponent>();
        if (!arrow_ptr) continue;
        const auto& arr = *arrow_ptr;

        uint32_t first = static_cast<uint32_t>(vertices_.size());

        // Arrow shaft: local -Z direction (forward)
        glm::vec3 tip{0.0f, 0.0f, -arr.length};
        add_line({0.0f, 0.0f, 0.0f}, tip);

        // Arrowhead: two angled lines
        float head = arr.length * 0.2f;
        add_line(tip, {head * 0.3f, 0.0f, -arr.length + head});
        add_line(tip, {-head * 0.3f, 0.0f, -arr.length + head});
        add_line(tip, {0.0f, head * 0.3f, -arr.length + head});
        add_line(tip, {0.0f, -head * 0.3f, -arr.length + head});

        uint32_t count = static_cast<uint32_t>(vertices_.size()) - first;
        draw_commands_.push_back({obj->world_matrix_, first, count});
    }

    if (vertices_.empty()) return;

    // Upload to GPU
    VkDeviceSize required = vertices_.size() * sizeof(glm::vec3);
    if (!vertex_buffer_ || required > buffer_capacity_) {
        // Round up to avoid frequent reallocations
        buffer_capacity_ = std::max(required, VkDeviceSize{4096});
        vertex_buffer_ = std::make_unique<Buffer>(
            device_, physical_device_, buffer_capacity_,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vertex_buffer_->map();
    }
    vertex_buffer_->write(vertices_.data(), required);
}

// ── Draw ────────────────────────────────────────────────────────────────

void DebugDraw::draw(VkCommandBuffer cmd, VkPipelineLayout layout) const {
    if (!vertex_buffer_ || draw_commands_.empty()) return;

    VkBuffer vb = vertex_buffer_->handle();
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);

    for (const auto& dc : draw_commands_) {
        DebugPushConstants push{};
        push.model = dc.model;
        push.color = {0.0f, 1.0f, 0.0f, 1.0f};

        vkCmdPushConstants(cmd, layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(DebugPushConstants), &push);
        vkCmdDraw(cmd, dc.vertex_count, 1, dc.first_vertex, 0);
    }
}

} // namespace fjell
