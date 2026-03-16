#include "renderer/material.hpp"
#include "renderer/texture.hpp"

#include <array>
#include <stdexcept>

namespace fjell {

Material::Material(VkDevice device, VkDescriptorPool pool,
                   VkDescriptorSetLayout layout,
                   const Texture& base_color, const Texture& normal_map,
                   const Texture& metallic_roughness, const Texture& emissive,
                   PbrParams params)
    : params_{params} {
    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = pool;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &layout;

    if (vkAllocateDescriptorSets(device, &alloc_info, &descriptor_set_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate material descriptor set");
    }

    std::array<VkDescriptorImageInfo, 4> image_infos{};
    const Texture* textures[] = {&base_color, &normal_map, &metallic_roughness, &emissive};

    for (int i = 0; i < 4; ++i) {
        image_infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        image_infos[i].imageView = textures[i]->image_view();
        image_infos[i].sampler = textures[i]->sampler();
    }

    std::array<VkWriteDescriptorSet, 4> writes{};
    for (int i = 0; i < 4; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = descriptor_set_;
        writes[i].dstBinding = static_cast<uint32_t>(i);
        writes[i].dstArrayElement = 0;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].descriptorCount = 1;
        writes[i].pImageInfo = &image_infos[i];
    }

    vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
}

void Material::bind(VkCommandBuffer cmd, VkPipelineLayout pipeline_layout) const {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
                            1, 1, &descriptor_set_, 0, nullptr);
}

} // namespace fjell
