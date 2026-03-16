#pragma once

#include "renderer/uniform_types.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

namespace fjell {

class Texture;

class Material {
public:
    Material(VkDevice device, VkDescriptorPool pool,
             VkDescriptorSetLayout layout,
             const Texture& base_color, const Texture& normal_map,
             const Texture& metallic_roughness, const Texture& emissive,
             PbrParams params = {});

    void bind(VkCommandBuffer cmd, VkPipelineLayout pipeline_layout) const;

    [[nodiscard]] const PbrParams& params() const { return params_; }

private:
    VkDescriptorSet descriptor_set_{VK_NULL_HANDLE};
    PbrParams params_;
};

} // namespace fjell
