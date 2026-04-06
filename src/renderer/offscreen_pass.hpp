#pragma once

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>

namespace fjell {

class Device;

class OffscreenPass {
public:
    OffscreenPass(Device& device, VmaAllocator allocator,
                  uint32_t width, uint32_t height,
                  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT);
    ~OffscreenPass();

    OffscreenPass(const OffscreenPass&) = delete;
    OffscreenPass& operator=(const OffscreenPass&) = delete;
    OffscreenPass(OffscreenPass&&) = delete;
    OffscreenPass& operator=(OffscreenPass&&) = delete;

    void resize(uint32_t width, uint32_t height);

    // Returns the single-sample color view (resolve target when MSAA, direct when 1x).
    // Use this for post-process sampling and ImGui display.
    [[nodiscard]] VkImageView color_view() const { return resolve_color_view_ ? resolve_color_view_ : color_view_; }

    // Returns the MSAA color view for rendering into (same as color_view when 1x).
    [[nodiscard]] VkImageView msaa_color_view() const { return color_view_; }

    // Returns the single-sample depth view (resolve target when MSAA, direct when 1x).
    [[nodiscard]] VkImageView depth_view() const { return resolve_depth_view_ ? resolve_depth_view_ : depth_view_; }

    // Returns the MSAA depth view for rendering into (same as depth_view when 1x).
    [[nodiscard]] VkImageView msaa_depth_view() const { return depth_view_; }
    [[nodiscard]] VkImage color_image() const { return resolve_color_image_ ? resolve_color_image_ : color_image_; }
    [[nodiscard]] VkImage msaa_color_image() const { return color_image_; }
    [[nodiscard]] VkImage depth_image() const { return resolve_depth_image_ ? resolve_depth_image_ : depth_image_; }
    [[nodiscard]] VkImage msaa_depth_image() const { return depth_image_; }
    [[nodiscard]] VkExtent2D extent() const { return extent_; }
    [[nodiscard]] VkSampleCountFlagBits sample_count() const { return msaa_samples_; }
    [[nodiscard]] VkDescriptorSet imgui_texture();
    [[nodiscard]] VkDescriptorSet depth_imgui_texture();
    void invalidate_imgui_texture();
    void invalidate_depth_imgui_texture();

    static constexpr VkFormat COLOR_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat MR_FORMAT = VK_FORMAT_R8G8_UNORM;

    // Returns the single-sample metallic/roughness view (resolve target when MSAA, direct when 1x).
    [[nodiscard]] VkImageView mr_view() const { return resolve_mr_view_ ? resolve_mr_view_ : mr_view_; }

    // Returns the MSAA metallic/roughness view for rendering into (same as mr_view when 1x).
    [[nodiscard]] VkImageView msaa_mr_view() const { return mr_view_; }

    [[nodiscard]] VkImage mr_image() const { return resolve_mr_image_ ? resolve_mr_image_ : mr_image_; }
    [[nodiscard]] VkImage msaa_mr_image() const { return mr_image_; }

    // Screen color copy — snapshot of resolved color after opaques, before transparents.
    [[nodiscard]] VkImage screen_color_image() const { return screen_color_image_; }
    [[nodiscard]] VkImageView screen_color_view() const { return screen_color_view_; }
    [[nodiscard]] bool screen_color_needs_init() const { return screen_color_needs_init_; }
    void mark_screen_color_initialized() { screen_color_needs_init_ = false; }

private:
    void create(uint32_t width, uint32_t height);
    void cleanup_images();

    Device& device_;
    VmaAllocator allocator_{VK_NULL_HANDLE};
    VkSampleCountFlagBits msaa_samples_{VK_SAMPLE_COUNT_1_BIT};

    // Color render target (MSAA when samples > 1)
    VkImage color_image_{VK_NULL_HANDLE};
    VmaAllocation color_allocation_{VK_NULL_HANDLE};
    VkImageView color_view_{VK_NULL_HANDLE};

    // Resolve target (single-sample, only created when MSAA)
    VkImage resolve_color_image_{VK_NULL_HANDLE};
    VmaAllocation resolve_color_allocation_{VK_NULL_HANDLE};
    VkImageView resolve_color_view_{VK_NULL_HANDLE};

    VkSampler sampler_{VK_NULL_HANDLE};
    VkSampler depth_sampler_{VK_NULL_HANDLE};

    // Depth render target (MSAA when samples > 1)
    VkImage depth_image_{VK_NULL_HANDLE};
    VmaAllocation depth_allocation_{VK_NULL_HANDLE};
    VkImageView depth_view_{VK_NULL_HANDLE};

    // Depth resolve target (single-sample, only created when MSAA)
    VkImage resolve_depth_image_{VK_NULL_HANDLE};
    VmaAllocation resolve_depth_allocation_{VK_NULL_HANDLE};
    VkImageView resolve_depth_view_{VK_NULL_HANDLE};

    // Metallic/roughness render target (MSAA when samples > 1)
    VkImage mr_image_{VK_NULL_HANDLE};
    VmaAllocation mr_allocation_{VK_NULL_HANDLE};
    VkImageView mr_view_{VK_NULL_HANDLE};

    // Metallic/roughness resolve target (single-sample, only created when MSAA)
    VkImage resolve_mr_image_{VK_NULL_HANDLE};
    VmaAllocation resolve_mr_allocation_{VK_NULL_HANDLE};
    VkImageView resolve_mr_view_{VK_NULL_HANDLE};

    // Screen color copy (single-sample, for SCREEN_TEXTURE in transparent shaders)
    VkImage screen_color_image_{VK_NULL_HANDLE};
    VmaAllocation screen_color_allocation_{VK_NULL_HANDLE};
    VkImageView screen_color_view_{VK_NULL_HANDLE};
    bool screen_color_needs_init_{true};

    VkExtent2D extent_{};

    VkDescriptorSet imgui_descriptor_{VK_NULL_HANDLE};
    VkDescriptorSet depth_imgui_descriptor_{VK_NULL_HANDLE};
};

} // namespace fjell
