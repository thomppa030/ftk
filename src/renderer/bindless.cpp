#include "renderer/bindless.hpp"
#include "gpu/vulkan/native.hpp"
#include "renderer/resources/texture.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"

#include <array>
#include <cstring>

namespace fjell {

Result<> BindlessTextures::create(VkDevice device, VmaAllocator allocator, gpu::Device& gpu) {
    device_ = device;
    allocator_ = allocator;
    gpu_device_ = &gpu;

    // Descriptor set layout: binding 0 = texture array, binding 1 = material SSBO,
    // binding 2 = custom params SSBO
    std::array<VkDescriptorSetLayoutBinding, 3> bindings{};

    // Compute is in the list for the fog material, whose `shader_type fog`
    // shader reads its textures and parameters from this set inside the
    // volumetric fog dispatch, and for the ray traced passes, which read the
    // material at a hit from their compute traces.
    constexpr VkShaderStageFlags all_stages =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
        VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = MAX_TEXTURES;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

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
    layout_bindings_ = bindings;

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
    // Empty until update_entry() gives it a texture: update_descriptors()
    // leaves it unwritten, which the partially bound array allows.
    entries_.push_back(Entry{VK_NULL_HANDLE, VK_NULL_HANDLE});
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

    // Reserved up front: a reallocation would move what pImageInfo points at.
    std::vector<VkDescriptorImageInfo> image_infos;
    std::vector<VkWriteDescriptorSet> writes;
    image_infos.reserve(count);
    writes.reserve(count);

    for (uint32_t idx = tex_dirty_from_; idx < entries_.size(); ++idx) {
        const Entry& entry = entries_[idx];
        // A reserved slot holds nothing until update_entry() fills it, and
        // the array is partially bound: it stays unwritten.
        if (entry.view == VK_NULL_HANDLE) continue;

        VkDescriptorImageInfo& info = image_infos.emplace_back();
        info.sampler = entry.sampler;
        info.imageView = entry.view;
        info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet& write = writes.emplace_back();
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set_;
        write.dstBinding = 0;
        write.dstArrayElement = idx;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &info;
    }

    if (!writes.empty()) {
        vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }

    tex_dirty_from_ = static_cast<uint32_t>(entries_.size());
}

void BindlessTextures::update_material_buffer() {
    FJELL_PROFILE_SCOPE_N("bindless_update_materials");
    if (materials_dirty_ && !materials_.empty()) {
        // The rows are a growing vector and the buffer behind them is not:
        // copying all of them writes past a mapped GPU allocation the moment
        // there are more than it holds, which corrupts whatever follows and
        // surfaces later as a lost device rather than as anything to do with
        // materials. Copy what fits and say what was dropped.
        const std::size_t capacity = material_ssbo_->size() / sizeof(GpuMaterial);
        const std::size_t count = std::min(materials_.size(), capacity);
        if (materials_.size() > capacity && !material_overflow_reported_) {
            material_overflow_reported_ = true;
            FJELL_GFX_ERROR("Bindless materials full: {} rows for {} slots. "
                            "Everything past the limit draws with the wrong "
                            "material.", materials_.size(), capacity);
        }
        std::memcpy(material_ssbo_->mapped(), materials_.data(),
                    count * sizeof(GpuMaterial));
        materials_dirty_ = false;
    }

    if (params_dirty_ && !params_.empty()) {
        VkDeviceSize needed = params_.size() * sizeof(float);
        if (needed > params_ssbo_->size()) {
            // Grow the params SSBO and re-bind the descriptor. In-flight
            // frames still read the old buffer through the previous
            // descriptor — retire it instead of destroying it inline.
            gpu::vulkan::retire(*gpu_device_, std::move(params_ssbo_));
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
