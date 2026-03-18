#pragma once

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

namespace fjell {

class Device;
class DebugDraw;
class OffscreenPass;
class RenderQueue;
class Scene;

// Shared descriptor set layouts needed by passes at pipeline creation time.
struct PassResources {
    VkDescriptorSetLayout global_set_layout{VK_NULL_HANDLE};
    VkDescriptorSetLayout material_set_layout{VK_NULL_HANDLE};
    VkFormat depth_format{VK_FORMAT_UNDEFINED};
    VkFormat color_format{VK_FORMAT_UNDEFINED};
};

// Shared state that every pass can read during recording.
struct FrameContext {
    VkCommandBuffer cmd{VK_NULL_HANDLE};
    OffscreenPass* target{nullptr};
    const RenderQueue* queue{nullptr};
    VkDescriptorSet global_descriptor_set{VK_NULL_HANDLE};
    VkDescriptorSet bindless_set{VK_NULL_HANDLE};
    glm::vec3 camera_position{0.0f};
    DebugDraw* debug_draw{nullptr};
    const Scene* scene{nullptr};
    float delta_time{0.0f};
    uint32_t selected_id{0};
    bool show_colliders{false};
    uint32_t frame_index{0};
};

// Base class for a render pass. Each pass owns its pipeline(s) and
// knows how to record its commands into a command buffer.
class RenderPass {
public:
    virtual ~RenderPass() = default;

    RenderPass() = default;
    RenderPass(const RenderPass&) = delete;
    RenderPass& operator=(const RenderPass&) = delete;
    RenderPass(RenderPass&&) = delete;
    RenderPass& operator=(RenderPass&&) = delete;

    // Create GPU resources (pipelines, layouts).
    virtual void create(Device& device, const PassResources& resources) = 0;

    // Destroy GPU resources.
    virtual void destroy() = 0;

    // Record draw commands into the command buffer.
    virtual void record(const FrameContext& ctx) = 0;

    void recreate(Device& device, const PassResources& resources) {
        destroy();
        create(device, resources);
    }
};

} // namespace fjell
