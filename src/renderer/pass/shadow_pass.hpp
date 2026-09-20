#pragma once

#include "renderer/pass/depth_pipeline.hpp"
#include "renderer/pass/render_pass.hpp"

#include <glm/glm.hpp>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace fjell {

class Camera;

class ShadowPass : public RenderPass {
public:
    static constexpr uint32_t CASCADE_COUNT = FrameContext::MAX_SHADOW_CASCADES;
    static constexpr uint32_t MAP_SIZE = 4096;
    static constexpr VkFormat DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;
    /// Slope-scaled depth bias every caster is rasterized with. Shared so a
    /// pass that draws into a cascade with its own pipeline — terrain does —
    /// biases the same way; a cascade written half at one bias reads as bands
    /// across the ground.
    static constexpr float DEPTH_BIAS_CONSTANT = 1.25F;
    static constexpr float DEPTH_BIAS_SLOPE = 1.75F;

    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void collect_exports(DeclareContext& ctx) override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;
    void prepare_frame(const FrameContext& ctx, GpuContext& gpu) override;

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
    // Compute cascade splits and light-space matrices for this frame.
    void update_cascades(const glm::mat4& view, const glm::mat4& projection,
                         const glm::vec3& light_direction, float near, float far);
    void create_images();
    void destroy_images();

    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineCache vk_pipeline_cache_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};

    // Depth texture array: one layer per cascade
    VkImage image_{VK_NULL_HANDLE};
    VmaAllocation memory_{VK_NULL_HANDLE};
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
