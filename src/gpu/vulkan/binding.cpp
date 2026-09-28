#include "gpu/vulkan/device_impl.hpp"

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
constexpr std::array<VkDescriptorPoolSize, 6> POOL_SIZES{{
    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 512},
    {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 256},
    {VK_DESCRIPTOR_TYPE_SAMPLER, 128},
    {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256},
    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 256},
    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 512},
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

// The placed entries' resources must all exist before a set is written with
// them: a stale handle would write a null descriptor the GPU then reads.
Result<> check_resources(const Device::Impl& self, const PlacedSet& placed) {
    for (const auto& entry : placed.entries) {
        const BindResource& r = entry.resource;
        bool present = true;
        switch (r.kind) {
            case BindingKind::sampled_texture:
                present = self.textures.contains(r.view.texture) && self.samplers.contains(r.sampler);
                break;
            case BindingKind::texture:
            case BindingKind::storage_texture:
                present = self.textures.contains(r.view.texture);
                break;
            case BindingKind::sampler:
                present = self.samplers.contains(r.sampler);
                break;
            case BindingKind::uniform_buffer:
            case BindingKind::storage_buffer:
                present = self.buffers.contains(r.buffer.buffer);
                break;
            case BindingKind::acceleration_structure:
                return make_error("Binding " + std::to_string(entry.binding) +
                                  ": acceleration structures are not bound through groups yet");
        }
        if (!present) {
            return make_error("Binding " + std::to_string(entry.binding) + " of set " +
                              std::to_string(placed.set) + " is given a resource that no longer exists");
        }
    }
    return {};
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
            create_info.poolSizeCount = static_cast<uint32_t>(POOL_SIZES.size());
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

void Device::Impl::write_set(VkDescriptorSet set, const PlacedSet& placed) {
    // Reserved up front: the writes point into these.
    std::vector<VkDescriptorImageInfo> image_infos;
    std::vector<VkDescriptorBufferInfo> buffer_infos;
    image_infos.reserve(placed.entries.size());
    buffer_infos.reserve(placed.entries.size());
    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(placed.entries.size());

    for (const auto& entry : placed.entries) {
        const BindResource& r = entry.resource;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = entry.binding;
        write.dstArrayElement = entry.element;
        write.descriptorCount = 1;
        write.descriptorType = vulkan::to_vk(r.kind);

        auto view_format = [&] {
            const TextureRecord* texture = textures.get(r.view.texture);
            return resolve(r.view, texture->info).format;
        };
        const VkSampler* sampler = samplers.get(r.sampler);
        switch (r.kind) {
            case BindingKind::sampled_texture:
                image_infos.push_back({*sampler, image_view(r.view), vulkan::sampled_layout(view_format())});
                write.pImageInfo = &image_infos.back();
                break;
            case BindingKind::texture:
                image_infos.push_back({VK_NULL_HANDLE, image_view(r.view), vulkan::sampled_layout(view_format())});
                write.pImageInfo = &image_infos.back();
                break;
            case BindingKind::sampler:
                image_infos.push_back({*sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
                write.pImageInfo = &image_infos.back();
                break;
            case BindingKind::storage_texture:
                image_infos.push_back({VK_NULL_HANDLE, image_view(r.view), VK_IMAGE_LAYOUT_GENERAL});
                write.pImageInfo = &image_infos.back();
                break;
            case BindingKind::uniform_buffer:
            case BindingKind::storage_buffer:
                buffer_infos.push_back({buffers.get(r.buffer.buffer)->buffer, r.buffer.offset,
                                        r.buffer.size == BufferRange::REST ? VK_WHOLE_SIZE : r.buffer.size});
                write.pBufferInfo = &buffer_infos.back();
                break;
            case BindingKind::acceleration_structure:
                continue;
        }
        writes.push_back(write);
    }
    vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

// ── Device ──────────────────────────────────────────────────────────────

Result<Owned<BindGroup>> Device::create(const BindGroupDesc& desc) {
    Impl& self = *impl_;
    const std::string what = named("Bind group", desc.name);
    const Impl::PipelineRecord* pipeline = find_pipeline(self, desc.pipeline);
    if (pipeline == nullptr) return make_error(what + ": the pipeline does not exist");

    auto placed = place(pipeline->shader_layout, desc.entries);
    if (!placed) return make_error(what + ": " + placed.error());
    for (const auto& [shared, set] : pipeline->layout.shared_sets) {
        if (set == placed->set) {
            return make_error(what + ": set " + std::to_string(set) + " is the shared '" +
                              self.shared_layouts.get(shared)->desc.name +
                              "', bound through the engine's shared group");
        }
    }
    if (auto present = check_resources(self, *placed); !present) {
        return make_error(what + ": " + present.error());
    }

    Impl::GroupRecord record;
    record.layout = pipeline->layout.set_layouts[placed->set];
    std::tie(record.set, record.pool) = self.allocate_set(record.layout);
    if (record.set == VK_NULL_HANDLE) return make_error(what + ": no descriptor set could be allocated");
    self.write_set(record.set, *placed);
    record.bindings = bindings_of(pipeline->shader_layout, placed->set);
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
    if (auto present = check_resources(self, *placed); !present) return present;

    const auto [set, pool] = self.allocate_set(record->layout);
    if (set == VK_NULL_HANDLE) return make_error("No descriptor set could be allocated for the update");
    self.write_set(set, *placed);
    // Commands already recorded still bind the old set; it goes once the GPU
    // is done with them.
    self.core.deferred_deleter().defer(
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
    self.core.deferred_deleter().defer([dev = self.device, pool = record->pool, set = record->set] {
        vkFreeDescriptorSets(dev, pool, 1, &set);
    });
}

} // namespace fjell::gpu
