#pragma once

#include "renderer/pass/render_pass.hpp"

#include <glm/glm.hpp>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <string>

namespace fjell {

/// Generates a sky gradient cubemap (64x64 per face, mipmapped) and a
/// split-sum BRDF integration LUT (512x512 RG16F) for IBL.
class SkyCubemapPass : public RenderPass {
public:
    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext&) override {}
    [[nodiscard]] PassSetup setup() const override;
    void build(FrameGraph& graph, ResourceRegistry& registry,
               const FrameContext& ctx) override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;
    void register_resources(ResourceRegistry& registry) override;
    void prepare_frame(const FrameContext& ctx, GpuContext& gpu) override;

    // Legacy create — forwards to standard create (needs command pool + queue)
    void create(VkDevice device, VmaAllocator allocator,
                VkCommandPool command_pool, VkQueue queue,
                VkPipelineCache pipeline_cache = VK_NULL_HANDLE);

    /// Set sky gradient colors. Marks dirty so next generate() re-renders.
    void set_colors(const glm::vec3& top, const glm::vec3& bottom);

    /// Dispatch the cubemap compute shader and generate mips.
    /// Only does work if dirty.
    void generate(VkCommandBuffer cmd);

    [[nodiscard]] VkImageView cubemap_view() const {
        return hdri_cubemap_view_ != VK_NULL_HANDLE ? hdri_cubemap_view_ : cubemap_view_;
    }
    [[nodiscard]] VkSampler cubemap_sampler() const {
        return hdri_cubemap_sampler_ != VK_NULL_HANDLE ? hdri_cubemap_sampler_ : cubemap_sampler_;
    }
    [[nodiscard]] VkImageView brdf_lut_view() const { return brdf_lut_view_; }
    [[nodiscard]] VkSampler brdf_lut_sampler() const { return brdf_lut_sampler_; }

    [[nodiscard]] bool dirty() const { return dirty_; }
    [[nodiscard]] bool has_hdri() const { return hdri_cubemap_view_ != VK_NULL_HANDLE; }

    /// Load an HDRI equirectangular image, convert to cubemap, and prefilter.
    /// Path must be absolute or resolvable from the current working directory.
    void load_hdri(const std::string& path);

    void set_project(const class Project* p) { project_ = p; }

private:
    void create_cubemap();
    void create_brdf_lut();
    void create_cubemap_pipeline();
    void create_brdf_pipeline();
    void create_hdri_pipelines();
    void generate_brdf_lut();
    void generate_mips(VkCommandBuffer cmd);
    void generate_mips_hdri(VkCommandBuffer cmd);
    void destroy_hdri();

    static constexpr uint32_t CUBEMAP_SIZE = 64;
    static constexpr uint32_t HDRI_CUBEMAP_SIZE = 512;
    static constexpr uint32_t HDRI_MIP_LEVELS = 10; // floor(log2(512)) + 1
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
    const class Project* project_{nullptr};

    // ── HDRI environment map ──────────────────────────────────────────
    std::string loaded_hdri_path_;

    // Equirectangular source (2D, R32G32B32A32_SFLOAT)
    VkImage equirect_image_{VK_NULL_HANDLE};
    VmaAllocation equirect_memory_{VK_NULL_HANDLE};
    VkImageView equirect_view_{VK_NULL_HANDLE};
    VkSampler equirect_sampler_{VK_NULL_HANDLE};

    // HDRI cubemap (512x512, 10 mips, prefiltered)
    VkImage hdri_cubemap_image_{VK_NULL_HANDLE};
    VmaAllocation hdri_cubemap_memory_{VK_NULL_HANDLE};
    VkImageView hdri_cubemap_view_{VK_NULL_HANDLE};       // CUBE view (all mips)
    VkImageView hdri_storage_view_{VK_NULL_HANDLE};       // 2D_ARRAY (mip 0, for equirect conversion)
    VkSampler hdri_cubemap_sampler_{VK_NULL_HANDLE};

    // Per-mip storage views for prefilter output
    std::array<VkImageView, HDRI_MIP_LEVELS> hdri_mip_views_{};

    // Equirect-to-cube pipeline
    VkDescriptorSetLayout equirect_set_layout_{VK_NULL_HANDLE};
    VkDescriptorPool equirect_pool_{VK_NULL_HANDLE};
    VkDescriptorSet equirect_set_{VK_NULL_HANDLE};
    VkPipelineLayout equirect_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline equirect_pipeline_{VK_NULL_HANDLE};

    // Prefilter pipeline
    VkDescriptorSetLayout prefilter_set_layout_{VK_NULL_HANDLE};
    VkDescriptorPool prefilter_pool_{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, HDRI_MIP_LEVELS> prefilter_sets_{};
    VkPipelineLayout prefilter_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline prefilter_pipeline_{VK_NULL_HANDLE};

    bool hdri_pipelines_created_{false};
};

} // namespace fjell
