#include "gpu/vulkan/device_impl.hpp"

#include "core/log.hpp"
#include "gpu/vulkan/translate.hpp"
#include "renderer/gpu/gpu_core.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <tuple>

namespace fjell::gpu {

namespace {

// What one pool of persistent groups holds. Groups are few and long-lived;
// a full pool is joined by another.
constexpr uint32_t POOL_SETS = 256;
// The last only where the GPU has ray queries: a pool elsewhere may not name
// the type.
constexpr std::array<VkDescriptorPoolSize, 7> POOL_SIZES{{
    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 512},
    {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 256},
    {VK_DESCRIPTOR_TYPE_SAMPLER, 128},
    {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256},
    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 256},
    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 512},
    {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 32},
}};

std::string named(std::string_view what, std::string_view name) {
    std::string text(what);
    if (!name.empty()) text += " '" + std::string(name) + "'";
    return text;
}

// The one set's bindings, what the group's later updates are checked against.
ShaderLayout bindings_of(const ShaderLayout& layout, uint32_t set) {
    ShaderLayout out;
    out.stages = layout.stages;
    for (const auto& binding : layout.bindings) {
        if (binding.set == set) out.bindings.push_back(binding);
    }
    return out;
}

const Device::Impl::PipelineRecord* find_pipeline(const Device::Impl& self, const PipelineRef& ref) {
    if (ref.compute.valid()) return self.compute_pipelines.get(ref.compute);
    return self.graphics_pipelines.get(ref.graphics);
}

} // namespace

// ── Impl ────────────────────────────────────────────────────────────────

std::pair<VkDescriptorSet, VkDescriptorPool> Device::Impl::allocate_set(VkDescriptorSetLayout layout) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (group_pools.empty() || attempt == 1) {
            VkDescriptorPoolCreateInfo create_info{};
            create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            create_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
            create_info.maxSets = POOL_SETS;
            create_info.poolSizeCount = static_cast<uint32_t>(POOL_SIZES.size()) - (caps.ray_queries ? 0 : 1);
            create_info.pPoolSizes = POOL_SIZES.data();
            VkDescriptorPool pool{VK_NULL_HANDLE};
            if (vkCreateDescriptorPool(device, &create_info, nullptr, &pool) != VK_SUCCESS) break;
            group_pools.push_back(pool);
        }
        VkDescriptorSetAllocateInfo allocate_info{};
        allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocate_info.descriptorPool = group_pools.back();
        allocate_info.descriptorSetCount = 1;
        allocate_info.pSetLayouts = &layout;
        VkDescriptorSet set{VK_NULL_HANDLE};
        if (vkAllocateDescriptorSets(device, &allocate_info, &set) == VK_SUCCESS) {
            return {set, group_pools.back()};
        }
    }
    return {VK_NULL_HANDLE, VK_NULL_HANDLE};
}

Result<> Device::Impl::check_resources(const PlacedSet& placed) {
    for (const auto& entry : placed.entries) {
        const BindResource& r = entry.resource;
        bool present = true;
        switch (r.kind) {
            case BindingKind::sampled_texture:
                present = textures.contains(r.view.texture) && samplers.contains(r.sampler);
                break;
            case BindingKind::texture:
            case BindingKind::storage_texture:
                present = textures.contains(r.view.texture);
                break;
            case BindingKind::sampler:
                present = samplers.contains(r.sampler);
                break;
            case BindingKind::uniform_buffer:
            case BindingKind::storage_buffer:
                present = buffers.contains(r.buffer.buffer);
                break;
            case BindingKind::acceleration_structure:
                present = accelerations.contains(r.structure);
                break;
        }
        if (!present) {
            return make_error("Binding " + std::to_string(entry.binding) + " of set " +
                              std::to_string(placed.set) + " is given a resource that no longer exists");
        }
    }
    return {};
}

FrameCacheBinding Device::Impl::describe(const PlacedEntry& entry) {
    const BindResource& r = entry.resource;
    FrameCacheBinding described;
    described.binding = entry.binding;
    described.element = entry.element;
    described.type = vulkan::to_vk(r.kind);

    auto view_format = [&] { return resolve(r.view, textures.get(r.view.texture)->info).format; };
    const VkSampler* sampler = samplers.get(r.sampler);
    switch (r.kind) {
        case BindingKind::sampled_texture:
            described.image = {*sampler, image_view(r.view), vulkan::sampled_layout(view_format())};
            break;
        case BindingKind::texture:
            described.image = {VK_NULL_HANDLE, image_view(r.view), vulkan::sampled_layout(view_format())};
            break;
        case BindingKind::sampler:
            described.image = {*sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
            break;
        case BindingKind::storage_texture:
            described.image = {VK_NULL_HANDLE, image_view(r.view), VK_IMAGE_LAYOUT_GENERAL};
            break;
        case BindingKind::uniform_buffer:
        case BindingKind::storage_buffer:
            described.buffer = {buffers.get(r.buffer.buffer)->buffer, r.buffer.offset,
                                r.buffer.size == BufferRange::REST ? VK_WHOLE_SIZE : r.buffer.size};
            break;
        case BindingKind::acceleration_structure:
            described.acceleration = accelerations.get(r.structure)->structure;
            break;
    }
    return described;
}

void Device::Impl::write_set(VkDescriptorSet set, const PlacedSet& placed) {
    // Kept whole while the writes point into them.
    std::vector<FrameCacheBinding> described;
    described.reserve(placed.entries.size());
    for (const auto& entry : placed.entries) described.push_back(describe(entry));

    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(described.size());
    // Each acceleration structure's write points at one of these, kept
    // whole until the update.
    std::vector<VkWriteDescriptorSetAccelerationStructureKHR> structures;
    structures.reserve(described.size());
    for (const FrameCacheBinding& binding : described) {
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = binding.binding;
        write.dstArrayElement = binding.element;
        write.descriptorCount = 1;
        write.descriptorType = binding.type;
        if (binding.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
            binding.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            write.pBufferInfo = &binding.buffer;
        } else if (binding.type == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) {
            VkWriteDescriptorSetAccelerationStructureKHR& written = structures.emplace_back();
            written.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
            written.accelerationStructureCount = 1;
            written.pAccelerationStructures = &binding.acceleration;
            write.pNext = &written;
        } else {
            write.pImageInfo = &binding.image;
        }
        writes.push_back(write);
    }
    vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

std::string Device::Impl::shared_at(const PipelineRecord& pipeline, uint32_t set) {
    for (const auto& [shared, taken] : pipeline.layout.shared_sets) {
        if (taken == set) {
            return "set " + std::to_string(set) + " is the shared '" +
                   shared_layouts.get(shared)->desc.name + "', bound through the engine's shared group";
        }
    }
    return {};
}

void Device::Impl::report_once(const std::string& message) {
    std::lock_guard lock(reported_mutex);
    if (reported.insert(message).second) FJELL_GFX_ERROR("{}", message);
}

// ── Device ──────────────────────────────────────────────────────────────

Result<Owned<BindGroup>> Device::create(const BindGroupDesc& desc) {
    Impl& self = *impl_;
    const std::string what = named("Bind group", desc.name);
    const Impl::PipelineRecord* pipeline = find_pipeline(self, desc.pipeline);
    if (pipeline == nullptr) return make_error(what + ": the pipeline does not exist");

    auto placed = place(pipeline->shader_layout, desc.entries);
    if (!placed) return make_error(what + ": " + placed.error());
    if (const std::string shared = self.shared_at(*pipeline, placed->set); !shared.empty()) {
        return make_error(what + ": " + shared);
    }
    if (auto present = self.check_resources(*placed); !present) {
        return make_error(what + ": " + present.error());
    }

    Impl::GroupRecord record;
    record.layout = pipeline->layout.set_layouts[placed->set];
    std::tie(record.set, record.pool) = self.allocate_set(record.layout);
    if (record.set == VK_NULL_HANDLE) return make_error(what + ": no descriptor set could be allocated");
    self.write_set(record.set, *placed);
    record.bindings = bindings_of(pipeline->shader_layout, placed->set);
    record.set_index = placed->set;
    vulkan::name_object(self.device, VK_OBJECT_TYPE_DESCRIPTOR_SET, reinterpret_cast<uint64_t>(record.set),
                        desc.name);
    return Owned<BindGroup>(*this, self.groups.emplace(std::move(record)));
}

Result<> Device::update(BindGroup group, std::span<const BindEntry> entries) {
    Impl& self = *impl_;
    Impl::GroupRecord* record = self.groups.get(group);
    if (record == nullptr) return make_error("Updating a bind group that no longer exists");
    if (record->pool == VK_NULL_HANDLE) {
        return make_error("A shared group is written by the engine that owns it");
    }
    auto placed = place(record->bindings, entries);
    if (!placed) return std::unexpected(placed.error());
    if (auto present = self.check_resources(*placed); !present) return present;

    const auto [set, pool] = self.allocate_set(record->layout);
    if (set == VK_NULL_HANDLE) return make_error("No descriptor set could be allocated for the update");
    self.write_set(set, *placed);
    // Commands already recorded still bind the old set; it goes once the GPU
    // is done with them.
    self.release_later(
        [device = self.device, old_pool = record->pool, old_set = record->set] {
            vkFreeDescriptorSets(device, old_pool, 1, &old_set);
        });
    record->set = set;
    record->pool = pool;
    return {};
}

void release(Device& device, BindGroup group) {
    Device::Impl& self = device.impl();
    auto record = self.groups.take(group);
    if (!record.has_value() || record->pool == VK_NULL_HANDLE) return;
    self.release_later([dev = self.device, pool = record->pool, set = record->set] {
        vkFreeDescriptorSets(dev, pool, 1, &set);
    });
}

} // namespace fjell::gpu
