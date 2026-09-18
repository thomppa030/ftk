#include "renderer/global_bindings.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <unordered_map>

namespace fjell {

GlobalBindings::GlobalBindings() {
    register_builtins();
}

void GlobalBindings::register_builtins() {
    // The built-in bindings with their exact current numbers.
    // These must never change — shaders reference them by number.

    constexpr auto FRAG = VK_SHADER_STAGE_FRAGMENT_BIT;
    constexpr auto COMP = VK_SHADER_STAGE_COMPUTE_BIT;
    constexpr auto VERT = VK_SHADER_STAGE_VERTEX_BIT;
    constexpr auto TASK = VK_SHADER_STAGE_TASK_BIT_EXT;
    constexpr auto MESH = VK_SHADER_STAGE_MESH_BIT_EXT;
    constexpr auto ALL_GFX = VERT | FRAG | COMP | TASK | MESH;

    declare({"global_ubo",       0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         1, ALL_GFX});
    declare({"shadow_map",       1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, FRAG | COMP});
    declare({"light_ssbo",       2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         1, FRAG | COMP});
    declare({"tile_light_ssbo",  3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         1, FRAG | COMP});
    declare({"depth_texture",    4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, FRAG | COMP});
    declare({"ao_texture",       5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, FRAG});
    declare({"sky_cubemap",      6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, FRAG | COMP});
    declare({"brdf_lut",         7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, FRAG});
    declare({"screen_color",     8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, FRAG});
    declare({"scene_sdf",        9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, FRAG | COMP});
    declare({"gi_indirect",     10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, FRAG});
    declare({"local_shadow_atlas", 11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, FRAG | COMP});
    declare({"ss_gi_emissive",  12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, FRAG});
    declare({"contact_shadow",  13, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, FRAG});
    declare({"environment_ubo", 14, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         1, ALL_GFX});
    declare({"cloud_shadow",    15, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, FRAG | COMP});
    declare({"motion_field",    16, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, ALL_GFX});
    declare({"fog_volume_ssbo", 17, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         1, FRAG | COMP});
    declare({"fog_density_field", 18, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, FRAG | COMP});
}

void GlobalBindings::declare(GlobalBindingDecl decl) {
    assert(!finalized_ && "GlobalBindings::declare() called after finalize()");

    // Check for duplicate names
    if (name_to_binding_.contains(decl.name)) {
        throw std::runtime_error("GlobalBindings: duplicate binding name '" + decl.name + "'");
    }

    // Resolve auto-assignment
    if (decl.binding == BINDING_AUTO) {
        decl.binding = next_auto_binding_;
    }

    // Check for duplicate binding numbers
    for (const auto& e : entries_) {
        if (e.decl.binding == decl.binding) {
            throw std::runtime_error(
                "GlobalBindings: binding " + std::to_string(decl.binding) +
                " already used by '" + e.decl.name + "', cannot assign to '" + decl.name + "'");
        }
    }

    name_to_binding_[decl.name] = decl.binding;

    // Track next auto slot (always above the highest used)
    if (decl.binding >= next_auto_binding_) {
        next_auto_binding_ = decl.binding + 1;
    }

    entries_.push_back({std::move(decl)});
}

void GlobalBindings::finalize(VkDevice device, uint32_t frames_in_flight) {
    assert(!finalized_ && "GlobalBindings::finalize() called twice");
    assert(!entries_.empty() && "GlobalBindings: no bindings declared");

    device_ = device;

    // Sort entries by binding number for clean layout creation
    std::sort(entries_.begin(), entries_.end(),
              [](const auto& a, const auto& b) {
                  return a.decl.binding < b.decl.binding;
              });

    // Build VkDescriptorSetLayoutBinding array
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    bindings.reserve(entries_.size());
    for (const auto& e : entries_) {
        VkDescriptorSetLayoutBinding b{};
        b.binding = e.decl.binding;
        b.descriptorType = e.decl.type;
        b.descriptorCount = e.decl.count;
        b.stageFlags = e.decl.stages;
        bindings.push_back(b);
    }

    layout_ = std::make_unique<DescriptorSetLayout>(device, std::span{bindings});

    // Compute pool sizes from actual bindings
    std::unordered_map<VkDescriptorType, uint32_t> type_counts;
    for (const auto& e : entries_) {
        type_counts[e.decl.type] += e.decl.count;
    }

    std::vector<VkDescriptorPoolSize> pool_sizes;
    pool_sizes.reserve(type_counts.size());
    for (const auto& [type, count] : type_counts) {
        pool_sizes.push_back({type, count * frames_in_flight});
    }

    pool_ = std::make_unique<DescriptorPool>(device, std::span{pool_sizes}, frames_in_flight);

    // Allocate per-frame descriptor sets
    for (uint32_t i = 0; i < frames_in_flight; ++i) {
        descriptor_sets_[i] = pool_->allocate(*layout_);
    }

    // Auxiliary pool for preview sets — freeable, so previews can return their
    // set on teardown. Sized for a handful of concurrent offscreen viewports;
    // one set's worth of each descriptor type, times AUX_SET_CAPACITY.
    constexpr uint32_t AUX_SET_CAPACITY = 16;
    std::vector<VkDescriptorPoolSize> aux_sizes;
    aux_sizes.reserve(type_counts.size());
    for (const auto& [type, count] : type_counts) {
        aux_sizes.push_back({type, count * AUX_SET_CAPACITY});
    }
    VkDescriptorPoolCreateInfo aux_ci{};
    aux_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    aux_ci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    aux_ci.maxSets = AUX_SET_CAPACITY;
    aux_ci.poolSizeCount = static_cast<uint32_t>(aux_sizes.size());
    aux_ci.pPoolSizes = aux_sizes.data();
    if (vkCreateDescriptorPool(device, &aux_ci, nullptr, &aux_pool_) != VK_SUCCESS) {
        throw std::runtime_error("GlobalBindings: failed to create auxiliary descriptor pool");
    }

    finalized_ = true;

    // Log binding table
    FJELL_GFX_DEBUG("Global bindings finalized: {} bindings", entries_.size());
    for (const auto& e : entries_) {
        const char* type_str = "unknown";
        switch (e.decl.type) {
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:         type_str = "UBO"; break;
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: type_str = "IMAGE_SAMPLER"; break;
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:         type_str = "SSBO"; break;
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:          type_str = "STORAGE_IMAGE"; break;
            default: break;
        }
        FJELL_GFX_DEBUG("  [{}] {} - {}", e.decl.binding, e.decl.name, type_str);
    }
}

uint32_t GlobalBindings::binding_number(const std::string& name) const {
    auto it = name_to_binding_.find(name);
    assert(it != name_to_binding_.end() && "GlobalBindings: unknown binding name");
    return it->second;
}

bool GlobalBindings::has_binding(const std::string& name) const {
    return name_to_binding_.contains(name);
}

void GlobalBindings::write_image(uint32_t frame_index, const std::string& name,
                                  VkImageView view, VkSampler sampler,
                                  VkImageLayout image_layout) {
    assert(finalized_ && "GlobalBindings::write_image() called before finalize()");

    auto it = name_to_binding_.find(name);
    assert(it != name_to_binding_.end() && "GlobalBindings: unknown binding name for write");

    // Find the entry to get the descriptor type
    uint32_t binding = it->second;
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    for (const auto& e : entries_) {
        if (e.decl.binding == binding) {
            type = e.decl.type;
            break;
        }
    }

    auto& info = pending_image_infos_.emplace_back();
    info.sampler = sampler;
    info.imageView = view;
    info.imageLayout = image_layout;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptor_sets_[frame_index];
    write.dstBinding = binding;
    write.descriptorType = type;
    write.descriptorCount = 1;
    write.pImageInfo = &info;

    pending_writes_.push_back(write);
}

void GlobalBindings::write_buffer(uint32_t frame_index, const std::string& name,
                                   VkBuffer buffer, VkDeviceSize offset, VkDeviceSize range) {
    assert(finalized_ && "GlobalBindings::write_buffer() called before finalize()");

    auto it = name_to_binding_.find(name);
    assert(it != name_to_binding_.end() && "GlobalBindings: unknown binding name for write");

    uint32_t binding = it->second;
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    for (const auto& e : entries_) {
        if (e.decl.binding == binding) {
            type = e.decl.type;
            break;
        }
    }

    auto& info = pending_buffer_infos_.emplace_back();
    info.buffer = buffer;
    info.offset = offset;
    info.range = range;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptor_sets_[frame_index];
    write.dstBinding = binding;
    write.descriptorType = type;
    write.descriptorCount = 1;
    write.pBufferInfo = &info;

    pending_writes_.push_back(write);
}

void GlobalBindings::write_image_to(VkDescriptorSet set, const std::string& name,
                                     VkImageView view, VkSampler sampler,
                                     VkImageLayout image_layout) {
    assert(finalized_ && "GlobalBindings::write_image_to() called before finalize()");

    auto it = name_to_binding_.find(name);
    assert(it != name_to_binding_.end() && "GlobalBindings: unknown binding name for write");

    uint32_t binding = it->second;
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    for (const auto& e : entries_) {
        if (e.decl.binding == binding) {
            type = e.decl.type;
            break;
        }
    }

    auto& info = pending_image_infos_.emplace_back();
    info.sampler = sampler;
    info.imageView = view;
    info.imageLayout = image_layout;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorType = type;
    write.descriptorCount = 1;
    write.pImageInfo = &info;

    pending_writes_.push_back(write);
}

void GlobalBindings::flush_writes() {
    if (pending_writes_.empty()) return;

    vkUpdateDescriptorSets(device_,
                           static_cast<uint32_t>(pending_writes_.size()),
                           pending_writes_.data(), 0, nullptr);

    pending_writes_.clear();
    pending_image_infos_.clear();
    pending_buffer_infos_.clear();
}

VkDescriptorSet GlobalBindings::allocate_set() {
    assert(finalized_ && "GlobalBindings::allocate_set() called before finalize()");

    VkDescriptorSetLayout vk_layout = layout_->handle();
    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = aux_pool_;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &vk_layout;

    VkDescriptorSet set{VK_NULL_HANDLE};
    if (vkAllocateDescriptorSets(device_, &alloc_info, &set) != VK_SUCCESS) {
        throw std::runtime_error("GlobalBindings: failed to allocate auxiliary set "
                                 "(raise AUX_SET_CAPACITY?)");
    }
    return set;
}

void GlobalBindings::free_set(VkDescriptorSet set) {
    if (set == VK_NULL_HANDLE) return;
    vkFreeDescriptorSets(device_, aux_pool_, 1, &set);
}

void GlobalBindings::copy_from_frame(VkDescriptorSet dst, uint32_t src_frame_index) {
    assert(finalized_ && "GlobalBindings::copy_from_frame() called before finalize()");

    VkDescriptorSet src = descriptor_sets_[src_frame_index];

    std::vector<VkCopyDescriptorSet> copies;
    copies.reserve(entries_.size());
    for (const auto& e : entries_) {
        VkCopyDescriptorSet c{};
        c.sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET;
        c.srcSet = src;
        c.srcBinding = e.decl.binding;
        c.srcArrayElement = 0;
        c.dstSet = dst;
        c.dstBinding = e.decl.binding;
        c.dstArrayElement = 0;
        c.descriptorCount = e.decl.count;
        copies.push_back(c);
    }

    vkUpdateDescriptorSets(device_, 0, nullptr,
                           static_cast<uint32_t>(copies.size()), copies.data());
}

void GlobalBindings::destroy() {
    if (aux_pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, aux_pool_, nullptr);
        aux_pool_ = VK_NULL_HANDLE;
    }
    pool_.reset();
    layout_.reset();
    device_ = VK_NULL_HANDLE;
    finalized_ = false;
}

} // namespace fjell
