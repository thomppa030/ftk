#pragma once

#include "renderer/pass/render_pass.hpp"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>

namespace fjell {

class GtaoPass : public RenderPass {
public:
    // Per-viewport AO + blur images. Descriptor sets come from
    // FrameContext::descriptor_cache during record_into().
    struct Images {
        // AO output image (R8_UNORM, half-res)
        VkImage ao_image{VK_NULL_HANDLE};
        VmaAllocation ao_memory{VK_NULL_HANDLE};
        VkImageView ao_view{VK_NULL_HANDLE};

        // Blur temp image (R8_UNORM, half-res)
        VkImage blur_image{VK_NULL_HANDLE};
        VmaAllocation blur_memory{VK_NULL_HANDLE};
        VkImageView blur_view{VK_NULL_HANDLE};

        VkExtent2D extent{};  // half-res dimensions
        VkDescriptorSet imgui_descriptor{VK_NULL_HANDLE};
        bool images_need_init{true};

        void create(VkDevice device, VmaAllocator allocator,
                     uint32_t half_w, uint32_t half_h);
        void destroy(VkDevice device, VmaAllocator allocator);
        void resize(VkDevice device, VmaAllocator allocator,
                     uint32_t width, uint32_t height);

        [[nodiscard]] VkDescriptorSet imgui_texture(VkSampler sampler);
        void invalidate_imgui_texture();
    };

    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;

    // Record into externally-provided images.
    void record_into(const FrameContext& ctx, Images& imgs);

    void resize(uint32_t width, uint32_t height);

    [[nodiscard]] VkImageView ao_view() const { return default_images_.ao_view; }
    [[nodiscard]] VkImage ao_image() const { return default_images_.ao_image; }
    [[nodiscard]] VkSampler ao_sampler() const { return ao_sampler_; }

    [[nodiscard]] VkDescriptorSet imgui_texture();
    void invalidate_imgui_texture();

    void set_radius(float r) { radius_ = r; }
    void set_intensity(float i) { intensity_ = i; }
    void set_thickness(float t) { thickness_ = t; }
    void set_enabled(bool e) { enabled_ = e; }
    [[nodiscard]] float radius() const { return radius_; }
    [[nodiscard]] float intensity() const { return intensity_; }
    [[nodiscard]] float thickness() const { return thickness_; }
    [[nodiscard]] bool enabled() const { return enabled_; }

    // Expose layout/sampler so external Images can be created
    [[nodiscard]] VkDescriptorSetLayout image_set_layout() const { return pass_set_layout_; }

private:
    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineCache vk_pipeline_cache_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};

    // Resolution-independent (shared across all Images)
    VkSampler ao_sampler_{VK_NULL_HANDLE};
    VkDescriptorSetLayout pass_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout gtao_layout_{VK_NULL_HANDLE};
    VkPipeline gtao_pipeline_{VK_NULL_HANDLE};
    VkPipelineLayout blur_layout_{VK_NULL_HANDLE};
    VkPipeline blur_pipeline_{VK_NULL_HANDLE};

    // Parameters
    float radius_{1.0f};
    float intensity_{1.5f};
    float thickness_{0.5f};
    bool enabled_{true};

    // Default per-viewport images
    Images default_images_;
};

} // namespace fjell
