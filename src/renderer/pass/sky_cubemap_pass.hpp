#pragma once

#include "renderer/pass/render_pass.hpp"

#include <glm/glm.hpp>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>

namespace fjell {

/// Generates a sky gradient cubemap (64x64 per face, mipmapped) and a
/// split-sum BRDF integration LUT (512x512 RG16F) for IBL.
class SkyCubemapPass : public RenderPass {
public:
    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext&) override {}

    // Legacy create — forwards to standard create (needs command pool + queue)
    void create(VkDevice device, VmaAllocator allocator,
                VkCommandPool command_pool, VkQueue queue,
                VkPipelineCache pipeline_cache = VK_NULL_HANDLE);

    /// Set sky gradient colors. Marks dirty so next generate() re-renders.
    void set_colors(const glm::vec3& top, const glm::vec3& bottom);

    /// Dispatch the cubemap compute shader and generate mips.
    /// Only does work if dirty.
    void generate(VkCommandBuffer cmd);

    [[nodiscard]] VkImageView cubemap_view() const { return cubemap_view_; }
    [[nodiscard]] VkSampler cubemap_sampler() const { return cubemap_sampler_; }
    [[nodiscard]] VkImageView brdf_lut_view() const { return brdf_lut_view_; }
    [[nodiscard]] VkSampler brdf_lut_sampler() const { return brdf_lut_sampler_; }

    [[nodiscard]] bool dirty() const { return dirty_; }

private:
    void create_cubemap();
    void create_brdf_lut();
    void create_cubemap_pipeline();
    void create_brdf_pipeline();
    void generate_brdf_lut();
    void generate_mips(VkCommandBuffer cmd);

    static constexpr uint32_t CUBEMAP_SIZE = 64;
    static constexpr uint32_t BRDF_LUT_SIZE = 512;
    static constexpr uint32_t MIP_LEVELS = 7; // floor(log2(64)) + 1

    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineCache vk_pipeline_cache_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};
    VkCommandPool command_pool_{VK_NULL_HANDLE};
    VkQueue queue_{VK_NULL_HANDLE};

    // Cubemap
    VkImage cubemap_image_{VK_NULL_HANDLE};
    VmaAllocation cubemap_memory_{VK_NULL_HANDLE};
    VkImageView cubemap_view_{VK_NULL_HANDLE};        // CUBE view for sampling
    VkImageView cubemap_storage_view_{VK_NULL_HANDLE}; // 2D_ARRAY view for compute storage (mip 0)
    VkSampler cubemap_sampler_{VK_NULL_HANDLE};

    // Cubemap compute
    VkDescriptorSetLayout cubemap_set_layout_{VK_NULL_HANDLE};
    VkDescriptorPool cubemap_pool_{VK_NULL_HANDLE};
    VkDescriptorSet cubemap_set_{VK_NULL_HANDLE};
    VkPipelineLayout cubemap_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline cubemap_pipeline_{VK_NULL_HANDLE};

    // BRDF LUT
    VkImage brdf_lut_image_{VK_NULL_HANDLE};
    VmaAllocation brdf_lut_memory_{VK_NULL_HANDLE};
    VkImageView brdf_lut_view_{VK_NULL_HANDLE};
    VkSampler brdf_lut_sampler_{VK_NULL_HANDLE};

    // BRDF compute
    VkDescriptorSetLayout brdf_set_layout_{VK_NULL_HANDLE};
    VkDescriptorPool brdf_pool_{VK_NULL_HANDLE};
    VkDescriptorSet brdf_set_{VK_NULL_HANDLE};
    VkPipelineLayout brdf_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline brdf_pipeline_{VK_NULL_HANDLE};

    glm::vec3 top_color_{0.15f, 0.25f, 0.45f};
    glm::vec3 bottom_color_{0.35f, 0.35f, 0.4f};
    bool dirty_{true};
    bool first_generate_{true};
};

} // namespace fjell
