#pragma once

#include "renderer/buffer.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace fjell {

class Texture;

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

// Manages a single large descriptor set containing all textures and materials
// in the engine. Textures get a global index; materials are stored in an SSBO.
// Shaders access textures via textures[nonuniformEXT(index)] and materials
// via materials[push.material_id].
class BindlessTextures {
public:
    static constexpr uint32_t MAX_TEXTURES = 1024;
    static constexpr uint32_t MAX_MATERIALS = 1024;

    void create(VkDevice device, VkPhysicalDevice physical_device);
    void destroy();

    // Register a texture and get its bindless index.
    // Idempotent: re-adding the same texture returns the existing index.
    uint32_t add(const Texture& texture);

    // Register a material and get its index for push constants.
    uint32_t add_material(const GpuMaterial& mat);

    // Flush pending texture descriptor writes.
    void update_descriptors();

    // Flush pending material data to the SSBO.
    void update_material_buffer();

    [[nodiscard]] VkDescriptorSetLayout layout() const { return layout_; }
    [[nodiscard]] VkDescriptorSet descriptor_set() const { return set_; }

private:
    VkDevice device_{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device_{VK_NULL_HANDLE};

    VkDescriptorSetLayout layout_{VK_NULL_HANDLE};
    VkDescriptorPool pool_{VK_NULL_HANDLE};
    VkDescriptorSet set_{VK_NULL_HANDLE};
    VkSampler default_sampler_{VK_NULL_HANDLE};

    // Texture entries
    struct Entry {
        VkImageView view;
        VkSampler sampler;
    };
    std::vector<Entry> entries_;
    std::unordered_map<VkImageView, uint32_t> view_to_index_;
    uint32_t tex_dirty_from_{0};

    // Material SSBO
    std::vector<GpuMaterial> materials_;
    std::unique_ptr<Buffer> material_ssbo_;
    bool materials_dirty_{false};
};

} // namespace fjell
