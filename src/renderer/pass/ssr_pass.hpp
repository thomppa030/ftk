#pragma once

#include "renderer/pass/render_pass.hpp"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>

namespace fjell {

class SsrPass : public RenderPass {
public:
    // Per-viewport resolution-dependent image state.
    struct Images {
        VkImage ssr_image{VK_NULL_HANDLE};
        VmaAllocation ssr_memory{VK_NULL_HANDLE};
        VkImageView ssr_view{VK_NULL_HANDLE};
        VkExtent2D extent{};
        VkDescriptorSet imgui_descriptor{VK_NULL_HANDLE};

        // Own pool + descriptor sets
        VkDescriptorPool pool{VK_NULL_HANDLE};
        VkDescriptorSet trace_set{VK_NULL_HANDLE};
        VkDescriptorSet composite_set{VK_NULL_HANDLE};

        // Cached views for lazy descriptor updates
        VkImageView last_hiz_view{VK_NULL_HANDLE};
        VkImageView last_prev_color_view{VK_NULL_HANDLE};
        VkImageView last_color_view{VK_NULL_HANDLE};
        VkImageView last_mr_view{VK_NULL_HANDLE};

        void create(VkDevice device, VmaAllocator allocator,
                     VkDescriptorSetLayout trace_layout,
                     VkDescriptorSetLayout composite_layout,
                     VkSampler nearest_sampler,
                     uint32_t width, uint32_t height);
        void destroy(VkDevice device, VmaAllocator allocator);
        void resize(VkDevice device, VmaAllocator allocator,
                     VkDescriptorSetLayout trace_layout,
                     VkDescriptorSetLayout composite_layout,
                     VkSampler nearest_sampler,
                     uint32_t width, uint32_t height);

        [[nodiscard]] VkDescriptorSet imgui_texture(VkSampler sampler);
        void invalidate_imgui_texture();
    };

    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void build(FrameGraph& graph, ResourceRegistry& registry,
               const FrameContext& ctx) override;

    // Record into externally-provided images.
    void record_into(const FrameContext& ctx, Images& imgs);

    void resize(uint32_t width, uint32_t height);

    // Previous-frame color buffer management (for SSR history).
    // Call ensure_prev_frame before rendering; call copy_prev_frame after.
    void ensure_prev_frame(VkDevice device, VmaAllocator allocator,
                           VkCommandPool command_pool, VkQueue queue,
                           uint32_t width, uint32_t height);
    void copy_prev_frame(VkCommandBuffer cmd, VkImage src_color, VkExtent2D extent);
    [[nodiscard]] VkImageView prev_frame_view() const { return prev_frame_view_; }

    [[nodiscard]] VkImageView ssr_view() const { return default_images_.ssr_view; }
    [[nodiscard]] VkImage ssr_image() const { return default_images_.ssr_image; }

    [[nodiscard]] VkDescriptorSet imgui_texture();
    void invalidate_imgui_texture();

    void set_enabled(bool e) { enabled_ = e; }
    void set_max_steps(uint32_t s) { max_steps_ = s; }
    void set_max_distance(float d) { max_distance_ = d; }
    void set_thickness(float t) { thickness_ = t; }
    void set_intensity(float i) { intensity_ = i; }
    [[nodiscard]] bool enabled() const { return enabled_; }
    [[nodiscard]] uint32_t max_steps() const { return max_steps_; }
    [[nodiscard]] float max_distance() const { return max_distance_; }
    [[nodiscard]] float thickness() const { return thickness_; }
    [[nodiscard]] float intensity() const { return intensity_; }

    // Expose layouts/samplers so external Images can be created
    [[nodiscard]] VkDescriptorSetLayout trace_set_layout() const { return trace_set_layout_; }
    [[nodiscard]] VkDescriptorSetLayout composite_set_layout() const { return composite_set_layout_; }
    [[nodiscard]] VkSampler nearest_sampler() const { return nearest_sampler_; }
    [[nodiscard]] VkSampler linear_sampler() const { return linear_sampler_; }

private:
    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineCache vk_pipeline_cache_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};

    // Resolution-independent (shared across all Images)
    VkSampler linear_sampler_{VK_NULL_HANDLE};
    VkSampler nearest_sampler_{VK_NULL_HANDLE};

    VkDescriptorSetLayout trace_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout trace_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline trace_pipeline_{VK_NULL_HANDLE};

    VkDescriptorSetLayout composite_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout composite_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline composite_pipeline_{VK_NULL_HANDLE};

    // Parameters
    bool enabled_{true};
    uint32_t max_steps_{64};
    float max_distance_{100.0f};
    float thickness_{0.3f};
    float intensity_{1.0f};

    // Default per-viewport images
    Images default_images_;

    // Previous-frame color buffer (singleton, main viewport only)
    VkImage prev_frame_image_{VK_NULL_HANDLE};
    VmaAllocation prev_frame_allocation_{VK_NULL_HANDLE};
    VkImageView prev_frame_view_{VK_NULL_HANDLE};
    VkExtent2D prev_frame_extent_{};
};

} // namespace fjell
