#pragma once

#include "renderer/pass/depth_pipeline.hpp"
#include "renderer/pass/render_pass.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace fjell {

struct DirectionalLight;
class Camera;

class ShadowPass : public RenderPass {
public:
    static constexpr uint32_t CASCADE_COUNT = 3;
    static constexpr uint32_t MAP_SIZE = 4096;
    static constexpr VkFormat DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;

    void create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;

    // Compute cascade splits and light-space matrices for this frame.
    // Call before record().
    void update(const glm::mat4& view, const glm::mat4& projection,
                const DirectionalLight& light, float near, float far);

    /// Create a custom shadow pipeline for a specific shader pipeline ID
    /// using a custom vertex shader SPV.
    void register_custom_shadow(uint32_t pipeline_id, const std::string& vert_spv_path);

    [[nodiscard]] VkImageView cascade_view(uint32_t index) const { return cascade_views_[index]; }
    [[nodiscard]] VkImageView array_view() const { return array_view_; }
    [[nodiscard]] VkSampler sampler() const { return sampler_; }
    [[nodiscard]] const std::array<glm::mat4, CASCADE_COUNT>& light_matrices() const {
        return light_matrices_;
    }
    [[nodiscard]] const std::array<float, CASCADE_COUNT>& cascade_splits() const {
        return cascade_splits_;
    }

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
    VkPipeline skinned_pipeline_{VK_NULL_HANDLE};
    VkDescriptorSetLayout draw_data_layout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout global_set_layout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout material_set_layout_{VK_NULL_HANDLE};

    std::unordered_map<uint32_t, VkPipeline> custom_shadow_pipelines_;
    DepthPipelineConfig shadow_config_;

    // Per-frame cascade data
    std::array<glm::mat4, CASCADE_COUNT> light_matrices_{};
    std::array<float, CASCADE_COUNT> cascade_splits_{};
};

} // namespace fjell
