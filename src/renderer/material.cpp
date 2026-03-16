#include "renderer/material.hpp"
#include "renderer/texture.hpp"

#include <stdexcept>

namespace fjell {

Material::Material(VkDevice device, VkDescriptorPool pool,
                   VkDescriptorSetLayout layout, const Texture& texture,
                   glm::vec4 tint)
    : tint_{tint} {
    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = pool;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &layout;

    if (vkAllocateDescriptorSets(device, &alloc_info, &descriptor_set_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate material descriptor set");
    }

    VkDescriptorImageInfo image_info{};
    image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    image_info.imageView = texture.image_view();
    image_info.sampler = texture.sampler();

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptor_set_;
    write.dstBinding = 0;
    write.dstArrayElement = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &image_info;

    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}

void Material::bind(VkCommandBuffer cmd, VkPipelineLayout pipeline_layout) const {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
                            1, 1, &descriptor_set_, 0, nullptr);
}

} // namespace fjell
