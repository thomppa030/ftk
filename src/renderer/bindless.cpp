#include "renderer/bindless.hpp"
#include "renderer/texture.hpp"
#include "core/log.hpp"

#include <array>
#include <cstring>
#include <stdexcept>

namespace fjell {

void BindlessTextures::create(VkDevice device, VkPhysicalDevice physical_device) {
    device_ = device;
    physical_device_ = physical_device;

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

    // Descriptor set layout: binding 0 = texture array, binding 1 = material SSBO
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};

    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = MAX_TEXTURES;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    // Partially bound + update-after-bind on textures, update-after-bind on SSBO
    std::array<VkDescriptorBindingFlags, 2> bind_flags{};
    bind_flags[0] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                    VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    bind_flags[1] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;

    VkDescriptorSetLayoutBindingFlagsCreateInfo flags_ci{};
    flags_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flags_ci.bindingCount = static_cast<uint32_t>(bind_flags.size());
    flags_ci.pBindingFlags = bind_flags.data();

    VkDescriptorSetLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_ci.pNext = &flags_ci;
    layout_ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layout_ci.bindingCount = static_cast<uint32_t>(bindings.size());
    layout_ci.pBindings = bindings.data();

    if (vkCreateDescriptorSetLayout(device_, &layout_ci, nullptr, &layout_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create bindless descriptor set layout");
    }

    // Descriptor pool: sampler array + storage buffer
    std::array<VkDescriptorPoolSize, 2> pool_sizes{};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sizes[0].descriptorCount = MAX_TEXTURES;
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_sizes[1].descriptorCount = 1;

    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pool_ci.maxSets = 1;
    pool_ci.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
    pool_ci.pPoolSizes = pool_sizes.data();

    if (vkCreateDescriptorPool(device_, &pool_ci, nullptr, &pool_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create bindless descriptor pool");
    }

    // Allocate descriptor set (no variable count — we use partially-bound instead)
    VkDescriptorSetAllocateInfo alloc_ci{};
    alloc_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_ci.descriptorPool = pool_;
    alloc_ci.descriptorSetCount = 1;
    alloc_ci.pSetLayouts = &layout_;

    if (vkAllocateDescriptorSets(device_, &alloc_ci, &set_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate bindless descriptor set");
    }

    // Pre-allocate material SSBO (96 bytes per material * 1024 = 96 KB)
    VkDeviceSize ssbo_size = sizeof(GpuMaterial) * MAX_MATERIALS;
    material_ssbo_ = std::make_unique<Buffer>(
        device_, physical_device_, ssbo_size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    material_ssbo_->map();

    // Write SSBO descriptor (binding 1)
    VkDescriptorBufferInfo buffer_info{};
    buffer_info.buffer = material_ssbo_->handle();
    buffer_info.offset = 0;
    buffer_info.range = ssbo_size;

    VkWriteDescriptorSet ssbo_write{};
    ssbo_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    ssbo_write.dstSet = set_;
    ssbo_write.dstBinding = 1;
    ssbo_write.dstArrayElement = 0;
    ssbo_write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ssbo_write.descriptorCount = 1;
    ssbo_write.pBufferInfo = &buffer_info;

    vkUpdateDescriptorSets(device_, 1, &ssbo_write, 0, nullptr);

    FJELL_GFX_DEBUG("Bindless registry created (max {} textures, {} materials)",
                    MAX_TEXTURES, MAX_MATERIALS);
}

void BindlessTextures::destroy() {
    material_ssbo_.reset();
    materials_.clear();
    if (pool_ != VK_NULL_HANDLE) { vkDestroyDescriptorPool(device_, pool_, nullptr); pool_ = VK_NULL_HANDLE; }
    if (layout_ != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device_, layout_, nullptr); layout_ = VK_NULL_HANDLE; }
    if (default_sampler_ != VK_NULL_HANDLE) { vkDestroySampler(device_, default_sampler_, nullptr); default_sampler_ = VK_NULL_HANDLE; }
    entries_.clear();
    view_to_index_.clear();
    set_ = VK_NULL_HANDLE;
}

uint32_t BindlessTextures::add(const Texture& texture) {
    VkImageView view = texture.image_view();

    // Idempotent: return existing index if already registered
    auto it = view_to_index_.find(view);
    if (it != view_to_index_.end()) {
        return it->second;
    }

    uint32_t index = static_cast<uint32_t>(entries_.size());

    Entry entry{};
    entry.view = view;
    entry.sampler = texture.sampler();
    entries_.push_back(entry);
    view_to_index_[view] = index;

    if (index < tex_dirty_from_) tex_dirty_from_ = index;

    return index;
}

uint32_t BindlessTextures::add_material(const GpuMaterial& mat) {
    uint32_t index = static_cast<uint32_t>(materials_.size());
    materials_.push_back(mat);
    materials_dirty_ = true;
    return index;
}

void BindlessTextures::update_descriptors() {
    if (entries_.empty() || tex_dirty_from_ >= entries_.size()) return;

    uint32_t count = static_cast<uint32_t>(entries_.size()) - tex_dirty_from_;

    // Pre-allocate to avoid reallocation (which would invalidate pImageInfo pointers)
    std::vector<VkDescriptorImageInfo> image_infos(count);
    std::vector<VkWriteDescriptorSet> writes(count);

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t idx = tex_dirty_from_ + i;

        image_infos[i].sampler = entries_[idx].sampler;
        image_infos[i].imageView = entries_[idx].view;
        image_infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set_;
        writes[i].dstBinding = 0;
        writes[i].dstArrayElement = idx;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].descriptorCount = 1;
        writes[i].pImageInfo = &image_infos[i];
    }

    vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);

    tex_dirty_from_ = static_cast<uint32_t>(entries_.size());
}

void BindlessTextures::update_material_buffer() {
    if (!materials_dirty_ || materials_.empty()) return;

    std::memcpy(material_ssbo_->mapped(), materials_.data(),
                materials_.size() * sizeof(GpuMaterial));
    materials_dirty_ = false;
}

} // namespace fjell
