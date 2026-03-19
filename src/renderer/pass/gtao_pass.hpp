#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

namespace fjell {

class GtaoPass {
public:
    void create(VkDevice device, VkPhysicalDevice physical_device,
                VkDescriptorSetLayout global_set_layout);
    void destroy();

    void resize(uint32_t width, uint32_t height);

    void dispatch(VkCommandBuffer cmd, VkDescriptorSet global_set,
                  uint32_t width, uint32_t height);

    [[nodiscard]] VkImageView ao_view() const { return ao_view_; }
    [[nodiscard]] VkImage ao_image() const { return ao_image_; }
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

private:
    void create_images(uint32_t width, uint32_t height);
    void destroy_images();
    void update_descriptors();

    VkDevice device_{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device_{VK_NULL_HANDLE};

    // AO output image (R8_UNORM)
    VkImage ao_image_{VK_NULL_HANDLE};
    VkDeviceMemory ao_memory_{VK_NULL_HANDLE};
    VkImageView ao_view_{VK_NULL_HANDLE};

    // Blur temp image (R8_UNORM)
    VkImage blur_image_{VK_NULL_HANDLE};
    VkDeviceMemory blur_memory_{VK_NULL_HANDLE};
    VkImageView blur_view_{VK_NULL_HANDLE};

    VkSampler ao_sampler_{VK_NULL_HANDLE};
    VkExtent2D extent_{};
    VkDescriptorSet imgui_descriptor_{VK_NULL_HANDLE};

    // GTAO compute pipeline
    VkPipelineLayout gtao_layout_{VK_NULL_HANDLE};
    VkPipeline gtao_pipeline_{VK_NULL_HANDLE};

    // Blur compute pipeline
    VkPipelineLayout blur_layout_{VK_NULL_HANDLE};
    VkPipeline blur_pipeline_{VK_NULL_HANDLE};

    // Per-pass descriptors (storage images for compute write/read)
    VkDescriptorSetLayout pass_set_layout_{VK_NULL_HANDLE};
    VkDescriptorPool pass_pool_{VK_NULL_HANDLE};
    VkDescriptorSet gtao_set_{VK_NULL_HANDLE};      // write ao_image_
    VkDescriptorSet blur_h_set_{VK_NULL_HANDLE};     // read ao_image_, write blur_image_
    VkDescriptorSet blur_v_set_{VK_NULL_HANDLE};     // read blur_image_, write ao_image_

    bool images_need_init_{true};

    // Parameters
    float radius_{1.0f};
    float intensity_{1.5f};
    float thickness_{0.5f};
    bool enabled_{true};
};

} // namespace fjell
