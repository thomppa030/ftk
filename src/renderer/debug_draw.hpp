#pragma once

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace fjell {

class Buffer;
class Scene;

class DebugDraw {
public:
    DebugDraw(VkDevice device, VkPhysicalDevice physical_device);

    void update(const Scene& scene);
    void draw(VkCommandBuffer cmd, VkPipelineLayout layout) const;

    [[nodiscard]] bool empty() const { return draw_commands_.empty(); }

private:
    struct DrawCommand {
        glm::mat4 model;
        uint32_t first_vertex;
        uint32_t vertex_count;
    };

    void generate_box(const glm::vec3& half_extents);
    void generate_sphere(float radius, uint32_t segments = 24);
    void generate_capsule(float radius, float height, uint32_t segments = 16);

    void add_line(const glm::vec3& a, const glm::vec3& b);
    void add_circle(const glm::vec3& center, const glm::vec3& axis_a,
                    const glm::vec3& axis_b, float radius, uint32_t segments);

    VkDevice device_;
    VkPhysicalDevice physical_device_;

    std::vector<glm::vec3> vertices_;
    std::vector<DrawCommand> draw_commands_;
    std::unique_ptr<Buffer> vertex_buffer_;
    VkDeviceSize buffer_capacity_{0};
};

} // namespace fjell
