#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace fjell {

class Texture;

// Manages a single large descriptor set containing all textures in the
// engine. Each texture gets a global index. Shaders access textures via
// textures[nonuniformEXT(index)].
class BindlessTextures {
public:
    static constexpr uint32_t MAX_TEXTURES = 1024;

    void create(VkDevice device, VkPhysicalDevice physical_device);
    void destroy();

    // Register a texture and get its bindless index
    uint32_t add(const Texture& texture);

    // Update after adding textures (writes descriptors)
    void update_descriptors();

    [[nodiscard]] VkDescriptorSetLayout layout() const { return layout_; }
    [[nodiscard]] VkDescriptorSet descriptor_set() const { return set_; }

private:
    VkDevice device_{VK_NULL_HANDLE};

    VkDescriptorSetLayout layout_{VK_NULL_HANDLE};
    VkDescriptorPool pool_{VK_NULL_HANDLE};
    VkDescriptorSet set_{VK_NULL_HANDLE};
    VkSampler default_sampler_{VK_NULL_HANDLE};

    struct Entry {
        VkImageView view;
        VkSampler sampler;
    };
    std::vector<Entry> entries_;
    uint32_t dirty_from_{0}; // first index that needs descriptor update
};

// Material data as stored in the GPU storage buffer.
// Indexed by material ID from push constants.
struct GpuMaterial {
    uint32_t base_color_idx{0};
    uint32_t normal_idx{0};
    uint32_t metallic_roughness_idx{0};
    uint32_t emissive_idx{0};
    float base_color_factor[4]{1.0f, 1.0f, 1.0f, 1.0f};
    float metallic_factor{1.0f};
    float roughness_factor{1.0f};
    float has_normal_map{0.0f};
    float padding{0.0f};
};

} // namespace fjell
