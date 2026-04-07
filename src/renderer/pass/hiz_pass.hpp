#pragma once

#include "renderer/pass/render_pass.hpp"
#include "renderer/gpu/buffer.hpp"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>

namespace fjell {

class HizPass : public RenderPass {
public:
    static constexpr uint32_t MAX_HIZ_MIPS = 12;

    // Per-viewport resolution-dependent image state.
    struct Images {
        VkImage hiz_image{VK_NULL_HANDLE};
        VmaAllocation hiz_memory{VK_NULL_HANDLE};
        VkImageView hiz_sampled_view{VK_NULL_HANDLE};
        std::array<VkImageView, MAX_HIZ_MIPS> hiz_mip_views{};
        uint32_t hiz_mip_count{0};
        uint32_t hiz_width{0};
        uint32_t hiz_height{0};

        // Own pool + per-mip descriptor sets
        VkDescriptorPool pool{VK_NULL_HANDLE};
        std::array<VkDescriptorSet, MAX_HIZ_MIPS> mip_sets{};

        // Cached depth view to avoid redundant descriptor updates
        VkImageView last_depth_view{VK_NULL_HANDLE};

        void create(VkDevice device, VmaAllocator allocator,
                     VkDescriptorSetLayout set_layout, VkSampler sampler,
                     uint32_t width, uint32_t height);
        void destroy(VkDevice device, VmaAllocator allocator);
        void resize(VkDevice device, VmaAllocator allocator,
                     VkDescriptorSetLayout set_layout, VkSampler sampler,
                     uint32_t width, uint32_t height);

        [[nodiscard]] bool ready() const { return hiz_image != VK_NULL_HANDLE; }
    };

    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;

    // Record into externally-provided images.
    void record_into(const FrameContext& ctx, Images& imgs);

    void resize(uint32_t width, uint32_t height);

    [[nodiscard]] VkImageView sampled_view() const { return default_images_.hiz_sampled_view; }
    [[nodiscard]] VkSampler sampler() const { return sampler_; }
    [[nodiscard]] uint32_t mip_count() const { return default_images_.hiz_mip_count; }
    [[nodiscard]] bool ready() const { return default_images_.ready(); }

    // Expose layout/sampler so external Images can be created
    [[nodiscard]] VkDescriptorSetLayout image_set_layout() const { return set_layout_; }

private:
    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineCache vk_pipeline_cache_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};

    // Resolution-independent (shared across all Images)
    VkSampler sampler_{VK_NULL_HANDLE};
    VkDescriptorSetLayout set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};

    // Default per-viewport images
    Images default_images_;
};

} // namespace fjell
