#include "renderer/bindless.hpp"
#include "renderer/texture.hpp"
#include "core/log.hpp"

#include <array>
#include <stdexcept>

namespace fjell {

void BindlessTextures::create(VkDevice device, VkPhysicalDevice /*physical_device*/) {
    device_ = device;

    // Default sampler for empty slots
    VkSamplerCreateInfo sampler_ci{};
    sampler_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_ci.magFilter = VK_FILTER_LINEAR;
    sampler_ci.minFilter = VK_FILTER_LINEAR;
    sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_ci.anisotropyEnable = VK_TRUE;
    sampler_ci.maxAnisotropy = 16.0f;
    if (vkCreateSampler(device_, &sampler_ci, nullptr, &default_sampler_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create bindless default sampler");
    }

    // Descriptor set layout: one variable-count sampler array
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = MAX_TEXTURES;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    // Enable partially bound + variable count
    VkDescriptorBindingFlags bind_flags =
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
        VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT |
        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;

    VkDescriptorSetLayoutBindingFlagsCreateInfo flags_ci{};
    flags_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flags_ci.bindingCount = 1;
    flags_ci.pBindingFlags = &bind_flags;

    VkDescriptorSetLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_ci.pNext = &flags_ci;
    layout_ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layout_ci.bindingCount = 1;
    layout_ci.pBindings = &binding;

    if (vkCreateDescriptorSetLayout(device_, &layout_ci, nullptr, &layout_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create bindless descriptor set layout");
    }

    // Descriptor pool
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = MAX_TEXTURES;

    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pool_ci.maxSets = 1;
    pool_ci.poolSizeCount = 1;
    pool_ci.pPoolSizes = &pool_size;

    if (vkCreateDescriptorPool(device_, &pool_ci, nullptr, &pool_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create bindless descriptor pool");
    }

    // Allocate with variable count
    uint32_t variable_count = MAX_TEXTURES;
    VkDescriptorSetVariableDescriptorCountAllocateInfo variable_ci{};
    variable_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO;
    variable_ci.descriptorSetCount = 1;
    variable_ci.pDescriptorCounts = &variable_count;

    VkDescriptorSetAllocateInfo alloc_ci{};
    alloc_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_ci.pNext = &variable_ci;
    alloc_ci.descriptorPool = pool_;
    alloc_ci.descriptorSetCount = 1;
    alloc_ci.pSetLayouts = &layout_;

    if (vkAllocateDescriptorSets(device_, &alloc_ci, &set_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate bindless descriptor set");
    }

    FJELL_GFX_DEBUG("Bindless texture registry created (max {} textures)", MAX_TEXTURES);
}

void BindlessTextures::destroy() {
    if (pool_ != VK_NULL_HANDLE) { vkDestroyDescriptorPool(device_, pool_, nullptr); pool_ = VK_NULL_HANDLE; }
    if (layout_ != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device_, layout_, nullptr); layout_ = VK_NULL_HANDLE; }
    if (default_sampler_ != VK_NULL_HANDLE) { vkDestroySampler(device_, default_sampler_, nullptr); default_sampler_ = VK_NULL_HANDLE; }
    entries_.clear();
    set_ = VK_NULL_HANDLE;
}

uint32_t BindlessTextures::add(const Texture& texture) {
    uint32_t index = static_cast<uint32_t>(entries_.size());

    Entry entry{};
    entry.view = texture.image_view();
    entry.sampler = texture.sampler();
    entries_.push_back(entry);

    // Mark dirty for next update
    if (index < dirty_from_) dirty_from_ = index;

    return index;
}

void BindlessTextures::update_descriptors() {
    if (entries_.empty()) return;

    std::vector<VkDescriptorImageInfo> image_infos;
    std::vector<VkWriteDescriptorSet> writes;

    for (uint32_t i = dirty_from_; i < entries_.size(); ++i) {
        VkDescriptorImageInfo info{};
        info.sampler = entries_[i].sampler;
        info.imageView = entries_[i].view;
        info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        image_infos.push_back(info);

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set_;
        write.dstBinding = 0;
        write.dstArrayElement = i;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &image_infos.back();
        writes.push_back(write);
    }

    if (!writes.empty()) {
        vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }

    dirty_from_ = static_cast<uint32_t>(entries_.size());
}

} // namespace fjell
