#pragma once

#include "renderer/pass/render_pass.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>

namespace fjell {

struct DirectionalLight;
class Camera;

class ShadowPass : public RenderPass {
public:
    static constexpr uint32_t CASCADE_COUNT = 3;
    static constexpr uint32_t MAP_SIZE = 2048;
    static constexpr VkFormat DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;

    void create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;

    // Compute cascade splits and light-space matrices for this frame.
    // Call before record().
    void update(const glm::mat4& view, const glm::mat4& projection,
                const DirectionalLight& light, float near, float far);

    [[nodiscard]] VkImageView cascade_view(uint32_t index) const { return cascade_views_[index]; }
    [[nodiscard]] VkImageView array_view() const { return array_view_; }
    [[nodiscard]] VkSampler sampler() const { return sampler_; }
    [[nodiscard]] const std::array<glm::mat4, CASCADE_COUNT>& light_matrices() const {
        return light_matrices_;
    }
    [[nodiscard]] const std::array<float, CASCADE_COUNT>& cascade_splits() const {
        return cascade_splits_;
    }

    void set_draw_data_layout(VkDescriptorSetLayout layout) { draw_data_layout_ = layout; }

private:
    void create_images();
    void destroy_images();

    VkDevice device_{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device_{VK_NULL_HANDLE};

    // Depth texture array: one layer per cascade
    VkImage image_{VK_NULL_HANDLE};
    VkDeviceMemory memory_{VK_NULL_HANDLE};
    VkImageView array_view_{VK_NULL_HANDLE};                          // all layers
    std::array<VkImageView, CASCADE_COUNT> cascade_views_{};          // per-layer
    VkSampler sampler_{VK_NULL_HANDLE};

    // Pipeline
    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};
    VkDescriptorSetLayout draw_data_layout_{VK_NULL_HANDLE};

    // Per-frame cascade data
    std::array<glm::mat4, CASCADE_COUNT> light_matrices_{};
    std::array<float, CASCADE_COUNT> cascade_splits_{};
};

} // namespace fjell
