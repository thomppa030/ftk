#pragma once

#include "renderer/pass/render_pass.hpp"
#include "renderer/renderer_constants.hpp"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

namespace fjell {

class TaaResolvePass : public RenderPass {
public:
    // Per-viewport ping-pong history textures.
    struct Images {
        VkImage history[2]{VK_NULL_HANDLE, VK_NULL_HANDLE};
        VmaAllocation history_memory[2]{VK_NULL_HANDLE, VK_NULL_HANDLE};
        VkImageView history_view[2]{VK_NULL_HANDLE, VK_NULL_HANDLE};
        VkExtent2D extent{};
        uint32_t write_index{0}; // ping-pong: write to [write_index], read from [1-write_index]
        bool needs_init{true};

        VkDescriptorPool pool{VK_NULL_HANDLE};
        VkDescriptorSet sets[MAX_FRAMES_IN_FLIGHT]{};

        void create(VkDevice device, VmaAllocator allocator,
                     VkDescriptorSetLayout set_layout,
                     uint32_t width, uint32_t height);
        void destroy(VkDevice device, VmaAllocator allocator);
        void resize(VkDevice device, VmaAllocator allocator,
                     VkDescriptorSetLayout set_layout,
                     uint32_t width, uint32_t height);
    };

    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void build(FrameGraph& graph, ResourceRegistry& registry,
               const FrameContext& ctx) override;

    void record_into(const FrameContext& ctx, Images& imgs);
    void resize(uint32_t width, uint32_t height);

    // The resolved output that downstream passes (tonemap) should read
    [[nodiscard]] VkImageView output_view() const;
    [[nodiscard]] VkImage output_image() const;

    [[nodiscard]] VkDescriptorSetLayout image_set_layout() const { return set_layout_; }
    [[nodiscard]] VkSampler sampler() const { return sampler_; }

private:
    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineCache vk_pipeline_cache_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};

    VkSampler sampler_{VK_NULL_HANDLE};
    VkDescriptorSetLayout set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};

    Images default_images_;
};

} // namespace fjell
