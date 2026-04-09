#include "renderer/bindless.hpp"
#include "renderer/resources/texture.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"

#include <array>
#include <cstring>

namespace fjell {

Result<> BindlessTextures::create(VkDevice device, VmaAllocator allocator) {
    device_ = device;
    allocator_ = allocator;

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
        return make_error("Failed to create bindless default sampler");
    }

    // Descriptor set layout: binding 0 = texture array, binding 1 = material SSBO,
    // binding 2 = custom params SSBO
    std::array<VkDescriptorSetLayoutBinding, 3> bindings{};

    constexpr VkShaderStageFlags all_stages =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
        VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;

    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = MAX_TEXTURES;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = all_stages;

    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = all_stages;

    // Partially bound + update-after-bind on textures, update-after-bind on SSBOs
    std::array<VkDescriptorBindingFlags, 3> bind_flags{};
    bind_flags[0] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                    VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    bind_flags[1] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    bind_flags[2] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;

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
        return make_error("Failed to create bindless descriptor set layout");
    }

    // Descriptor pool: sampler array + 2 storage buffers
    std::array<VkDescriptorPoolSize, 2> pool_sizes{};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sizes[0].descriptorCount = MAX_TEXTURES;
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_sizes[1].descriptorCount = 2;

    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pool_ci.maxSets = 1;
    pool_ci.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
    pool_ci.pPoolSizes = pool_sizes.data();

    if (vkCreateDescriptorPool(device_, &pool_ci, nullptr, &pool_) != VK_SUCCESS) {
        return make_error("Failed to create bindless descriptor pool");
    }

    // Allocate descriptor set
    VkDescriptorSetAllocateInfo alloc_ci{};
    alloc_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_ci.descriptorPool = pool_;
    alloc_ci.descriptorSetCount = 1;
    alloc_ci.pSetLayouts = &layout_;

    if (vkAllocateDescriptorSets(device_, &alloc_ci, &set_) != VK_SUCCESS) {
        return make_error("Failed to allocate bindless descriptor set");
    }

    // Pre-allocate material SSBO (96 bytes per material * 1024 = 96 KB)
    VkDeviceSize mat_ssbo_size = sizeof(GpuMaterial) * MAX_MATERIALS;
    material_ssbo_ = std::make_unique<Buffer>(
        allocator_, mat_ssbo_size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    material_ssbo_->map();
    std::memset(material_ssbo_->mapped(), 0, mat_ssbo_size);

    // Pre-allocate custom params SSBO
    VkDeviceSize params_ssbo_size = INITIAL_PARAMS_CAPACITY * sizeof(float);
    params_ssbo_ = std::make_unique<Buffer>(
        allocator_, params_ssbo_size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    params_ssbo_->map();
    // Initialize with a single zero so the buffer is never empty
    params_.push_back(0.0f);
    std::memcpy(params_ssbo_->mapped(), params_.data(), sizeof(float));

    // Write SSBO descriptors (binding 1 = materials, binding 2 = params)
    VkDescriptorBufferInfo mat_buffer_info{};
    mat_buffer_info.buffer = material_ssbo_->handle();
    mat_buffer_info.offset = 0;
    mat_buffer_info.range = mat_ssbo_size;

    VkDescriptorBufferInfo params_buffer_info{};
    params_buffer_info.buffer = params_ssbo_->handle();
    params_buffer_info.offset = 0;
    params_buffer_info.range = params_ssbo_size;

    std::array<VkWriteDescriptorSet, 2> ssbo_writes{};
    ssbo_writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    ssbo_writes[0].dstSet = set_;
    ssbo_writes[0].dstBinding = 1;
    ssbo_writes[0].dstArrayElement = 0;
    ssbo_writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ssbo_writes[0].descriptorCount = 1;
    ssbo_writes[0].pBufferInfo = &mat_buffer_info;

    ssbo_writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    ssbo_writes[1].dstSet = set_;
    ssbo_writes[1].dstBinding = 2;
    ssbo_writes[1].dstArrayElement = 0;
    ssbo_writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ssbo_writes[1].descriptorCount = 1;
    ssbo_writes[1].pBufferInfo = &params_buffer_info;

    vkUpdateDescriptorSets(device_, static_cast<uint32_t>(ssbo_writes.size()),
                           ssbo_writes.data(), 0, nullptr);

    FJELL_GFX_DEBUG("Bindless registry created (max {} textures, {} materials)",
                    MAX_TEXTURES, MAX_MATERIALS);
    return {};
}

void BindlessTextures::destroy() {
    params_ssbo_.reset();
    params_.clear();
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

uint32_t BindlessTextures::reserve_index() {
    uint32_t index = static_cast<uint32_t>(entries_.size());
    entries_.push_back(Entry{VK_NULL_HANDLE, VK_NULL_HANDLE});
    // Push dirty marker past the reserved slot so update_descriptors()
    // doesn't try to write a VK_NULL_HANDLE descriptor.
    tex_dirty_from_ = static_cast<uint32_t>(entries_.size());
    return index;
}

void BindlessTextures::update_entry(uint32_t index, VkImageView view, VkSampler sampler) {
    if (index >= entries_.size()) return;

    // Remove old view from dedup map
    auto& old = entries_[index];
    if (old.view != VK_NULL_HANDLE) {
        view_to_index_.erase(old.view);
    }

    old.view = view;
    old.sampler = sampler;

    if (view != VK_NULL_HANDLE) {
        view_to_index_[view] = index;
    }

    if (index < tex_dirty_from_) tex_dirty_from_ = index;
}

uint32_t BindlessTextures::add_material(const GpuMaterial& mat) {
    uint32_t index = static_cast<uint32_t>(materials_.size());
    materials_.push_back(mat);
    materials_dirty_ = true;
    return index;
}

void BindlessTextures::update_material(uint32_t index, const GpuMaterial& mat) {
    if (index < materials_.size()) {
        materials_[index] = mat;
        materials_dirty_ = true;
    }
}

uint32_t BindlessTextures::allocate_params(uint32_t count) {
    if (count == 0) return 0;
    uint32_t offset = static_cast<uint32_t>(params_.size());
    params_.resize(params_.size() + count, 0.0f);
    params_dirty_ = true;
    return offset;
}

void BindlessTextures::write_params(uint32_t offset, const float* data, uint32_t count) {
    if (count == 0) return;
    if (offset + count > params_.size()) {
        params_.resize(offset + count, 0.0f);
    }
    std::memcpy(params_.data() + offset, data, count * sizeof(float));
    params_dirty_ = true;
}

void BindlessTextures::update_descriptors() {
    FJELL_PROFILE_SCOPE_N("bindless_update_descriptors");
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
    FJELL_PROFILE_SCOPE_N("bindless_update_materials");
    if (materials_dirty_ && !materials_.empty()) {
        std::memcpy(material_ssbo_->mapped(), materials_.data(),
                    materials_.size() * sizeof(GpuMaterial));
        materials_dirty_ = false;
    }

    if (params_dirty_ && !params_.empty()) {
        VkDeviceSize needed = params_.size() * sizeof(float);
        if (needed > params_ssbo_->size()) {
            // Grow the params SSBO and re-bind descriptor
            VkDeviceSize new_size = needed * 2;
            params_ssbo_ = std::make_unique<Buffer>(
                allocator_, new_size,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VMA_MEMORY_USAGE_AUTO,
                VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
            params_ssbo_->map();

            VkDescriptorBufferInfo params_info{};
            params_info.buffer = params_ssbo_->handle();
            params_info.offset = 0;
            params_info.range = new_size;

            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = set_;
            write.dstBinding = 2;
            write.dstArrayElement = 0;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.descriptorCount = 1;
            write.pBufferInfo = &params_info;

            vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        }

        std::memcpy(params_ssbo_->mapped(), params_.data(), needed);
        params_dirty_ = false;
    }
}

} // namespace fjell
