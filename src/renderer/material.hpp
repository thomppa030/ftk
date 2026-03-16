#pragma once

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

namespace fjell {

class Texture;

class Material {
public:
    Material(VkDevice device, VkDescriptorPool pool,
             VkDescriptorSetLayout layout, const Texture& texture,
             glm::vec4 tint = {1.0f, 1.0f, 1.0f, 1.0f});

    void bind(VkCommandBuffer cmd, VkPipelineLayout pipeline_layout) const;

    [[nodiscard]] glm::vec4 tint() const { return tint_; }

private:
    VkDescriptorSet descriptor_set_{VK_NULL_HANDLE};
    glm::vec4 tint_;
};

} // namespace fjell
