#pragma once

#include "renderer/renderer_constants.hpp"

#include <glm/glm.hpp>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <array>
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

    /// Record debug primitives and upload them into the current frame's
    /// vertex buffer. `frame_index` is the slot in [0, MAX_FRAMES_IN_FLIGHT)
    /// whose fence has already been waited on by the caller.
    void update(const Scene& scene, uint32_t frame_index, uint32_t selected_id = 0,
                bool show_colliders = true, DebugDrawQueue* script_queue = nullptr);

    /// Draw with the buffer populated by the matching `update()` call.
    void draw(VkCommandBuffer cmd, VkPipelineLayout layout, uint32_t frame_index) const;

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

    // One vertex buffer per in-flight frame slot. Growth replaces only the
    // current frame's buffer, which has already been retired by its fence.
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> vertex_buffers_;
    std::array<VkDeviceSize, MAX_FRAMES_IN_FLIGHT> buffer_capacities_{};
};

} // namespace fjell
