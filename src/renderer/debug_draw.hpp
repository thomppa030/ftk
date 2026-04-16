#pragma once

#include <glm/glm.hpp>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace fjell {

class Buffer;
class DebugDrawQueue;
class Scene;
struct CameraNode;

class DebugDraw {
public:
    DebugDraw(VkDevice device, VmaAllocator allocator);

    void update(const Scene& scene, uint32_t selected_id = 0,
                bool show_colliders = true, DebugDrawQueue* script_queue = nullptr);
    void draw(VkCommandBuffer cmd, VkPipelineLayout layout) const;

    [[nodiscard]] bool empty() const { return draw_commands_.empty(); }

private:
    struct DrawCommand {
        glm::mat4 model;
        uint32_t first_vertex;
        uint32_t vertex_count;
        glm::vec4 color{0.0f, 1.0f, 0.0f, 1.0f}; // default green
    };

    void generate_box(const glm::vec3& half_extents);
    void generate_sphere(float radius, uint32_t segments = 24);
    void generate_capsule(float radius, float height, uint32_t segments = 16);
    void generate_frustum(const CameraNode& cam, float aspect);

    void add_line(const glm::vec3& a, const glm::vec3& b);
    void add_circle(const glm::vec3& center, const glm::vec3& axis_a,
                    const glm::vec3& axis_b, float radius, uint32_t segments);

    VkDevice device_;
    VmaAllocator allocator_;

    std::vector<glm::vec3> vertices_;
    std::vector<DrawCommand> draw_commands_;
    std::unique_ptr<Buffer> vertex_buffer_;
    VkDeviceSize buffer_capacity_{0};
};

} // namespace fjell
