#include "gpu/vulkan/device_impl.hpp"

#include "core/log.hpp"
#include "gpu/vulkan/translate.hpp"
#include "renderer/gpu/vk_check.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <tuple>

namespace fjell::gpu {

namespace {

// What one pool of persistent groups holds. Groups are few and long-lived;
// a full pool is joined by another.
constexpr uint32_t POOL_SETS = 256;
// Sets in one pool of a made shared layout: the version bound now, and those
// frames in flight still read, before another pool is needed.
constexpr uint32_t SHARED_POOL_SETS = 4;
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

// The index in a shared layout's bindings of binding number `binding`.
size_t binding_index(const Device::Impl::SharedRecord& shared, uint32_t binding) {
    for (size_t i = 0; i < shared.desc.bindings.size(); ++i) {
        if (shared.desc.bindings[i].binding == binding) return i;
    }
    return 0;
}

// Where a placed entry's element sits in a shared group's contents.
size_t content_index(const Device::Impl::SharedRecord& shared, const PlacedEntry& entry) {
    return shared.first[binding_index(shared, entry.binding)] + entry.element;
}

// Adds a descriptor type's count to what one set of a layout holds.
void add_size(std::vector<VkDescriptorPoolSize>& sizes, VkDescriptorType type, uint32_t count) {
    for (auto& size : sizes) {
        if (size.type == type) {
            size.descriptorCount += count;
            return;
        }
    }
    sizes.push_back({type, count});
}

bool gives_nothing(const BindResource& r) {
    return !r.view.texture.valid() && !r.sampler.valid() && !r.buffer.buffer.valid() &&
           !r.structure.valid();
}

// A shared group's placed entries split in two: the resources written, and
// the array elements given nothing, which are emptied.
struct SharedChanges {
    PlacedSet written;
    SmallVector<PlacedEntry, INLINE_SET_ENTRIES> emptied;
};

Result<SharedChanges> split_changes(Device::Impl& self, const Device::Impl::SharedRecord& shared,
                                    const PlacedSet& placed) {
    SharedChanges changes;
    changes.written.set = placed.set;
    for (const PlacedEntry& entry : placed.entries) {
        if (!gives_nothing(entry.resource)) {
            changes.written.entries.push_back(entry);
            continue;
        }
        const ShaderBinding& binding = shared.desc.bindings[binding_index(shared, entry.binding)];
        if (!binding.array) {
            return make_error("'" + binding.name + "' is given nothing; only an array's elements may be empty");
        }
        changes.emptied.push_back(entry);
    }
    if (auto present = self.check_resources(changes.written); !present) {
        return std::unexpected(present.error());
    }
    return changes;
}

void apply_contents(Device::Impl::SharedGroupState& state, const Device::Impl::SharedRecord& shared,
                    const SharedChanges& changes) {
    for (const PlacedEntry& entry : changes.written.entries) {
        state.contents[content_index(shared, entry)] = entry.resource;
    }
    for (const PlacedEntry& entry : changes.emptied) state.contents[content_index(shared, entry)].reset();
}

// A new version of a shared group whose set a frame still reads: a set of
// its own, written whole from the contents with `changes` over them.
Result<> write_version(Device::Impl& self, Device::Impl::GroupRecord& record,
                       Device::Impl::SharedRecord& shared, const SharedChanges& changes) {
    const auto [set, pool] = self.allocate_shared(shared);
    if (set == VK_NULL_HANDLE) return make_error("No descriptor set could be allocated for the update");

    Device::Impl::SharedGroupState& state = *record.state;
    apply_contents(state, shared, changes);
    PlacedSet whole;
    whole.set = changes.written.set;
    for (size_t i = 0; i < shared.desc.bindings.size(); ++i) {
        const ShaderBinding& binding = shared.desc.bindings[i];
        for (uint32_t element = 0; element < binding.count; ++element) {
            auto& content = state.contents[shared.first[i] + element];
            if (!content) continue;
            if (!self.exists(*content)) {
                self.report_once(named("Shared group", state.name) + ": '" + binding.name +
                                 "' element " + std::to_string(element) +
                                 " holds a resource that no longer exists; it is left empty");
                content.reset();
                continue;
            }
            whole.entries.push_back({binding.binding, element, *content});
        }
    }
    self.write_set(set, whole);
    vulkan::name_object(self.device, VK_OBJECT_TYPE_DESCRIPTOR_SET, reinterpret_cast<uint64_t>(set),
                        state.name);
    self.release_later([device = self.device, old_pool = record.pool, old_set = record.set] {
        vkFreeDescriptorSets(device, old_pool, 1, &old_set);
    });
    record.set = set;
    record.pool = pool;
    state.bound.store(Device::Impl::SharedGroupState::NEVER_BOUND, std::memory_order_relaxed);
    return {};
}

Result<> update_shared(Device::Impl& self, Device::Impl::GroupRecord& record,
                       std::span<const BindEntry> entries) {
    Device::Impl::SharedRecord& shared = *self.shared_layouts.get(record.shared);
    auto placed = place_some(shared.desc, entries);
    if (!placed) return std::unexpected(placed.error());
    auto changes = split_changes(self, shared, *placed);
    if (!changes) return std::unexpected(changes.error());

    // What the group already holds is not written again: a caller setting a
    // binding every frame costs nothing while it stays the same, and makes no
    // new version while a frame reads the set.
    const auto& contents = record.state->contents;
    SharedChanges needed;
    needed.written.set = changes->written.set;
    for (const PlacedEntry& entry : changes->written.entries) {
        if (contents[content_index(shared, entry)] != entry.resource) needed.written.entries.push_back(entry);
    }
    for (const PlacedEntry& entry : changes->emptied) {
        if (contents[content_index(shared, entry)]) needed.emptied.push_back(entry);
    }
    if (needed.written.entries.empty() && needed.emptied.empty()) return {};
    *changes = std::move(needed);

    // The current set is written in place while no frame the GPU has yet to
    // finish binds it: before any bind, or once the last frame binding it is
    // done.
    const uint64_t bound = record.state->bound.load(std::memory_order_relaxed);
    if (bound != Device::Impl::SharedGroupState::NEVER_BOUND) {
        uint64_t finished = 0;
        vk_check(vkGetSemaphoreCounterValue(self.device, self.frame_timeline, &finished),
                 "Failed to read the frame timeline");
        if (bound > finished) return write_version(self, record, shared, *changes);
    }
    // An emptied element keeps its old descriptor in place, which no shader
    // reads: the array is partially bound, and the next version leaves it
    // out.
    if (!changes->written.entries.empty()) self.write_set(record.set, changes->written);
    apply_contents(*record.state, shared, *changes);
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

std::pair<VkDescriptorSet, VkDescriptorPool> Device::Impl::allocate_shared(SharedRecord& shared) {
    VkDescriptorSetAllocateInfo allocate_info{};
    allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate_info.descriptorSetCount = 1;
    allocate_info.pSetLayouts = &shared.layout;
    // Every set of the layout is the same size, so a set freed anywhere
    // makes room for the next; the newest pool is the likeliest to have it.
    for (auto pool = shared.pools.rbegin(); pool != shared.pools.rend(); ++pool) {
        allocate_info.descriptorPool = *pool;
        VkDescriptorSet set{VK_NULL_HANDLE};
        if (vkAllocateDescriptorSets(device, &allocate_info, &set) == VK_SUCCESS) return {set, *pool};
    }

    VkDescriptorPoolCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    create_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    create_info.maxSets = SHARED_POOL_SETS;
    std::vector<VkDescriptorPoolSize> sizes = shared.set_sizes;
    for (auto& size : sizes) size.descriptorCount *= SHARED_POOL_SETS;
    create_info.poolSizeCount = static_cast<uint32_t>(sizes.size());
    create_info.pPoolSizes = sizes.data();
    VkDescriptorPool pool{VK_NULL_HANDLE};
    if (vkCreateDescriptorPool(device, &create_info, nullptr, &pool) != VK_SUCCESS) {
        return {VK_NULL_HANDLE, VK_NULL_HANDLE};
    }
    shared.pools.push_back(pool);
    allocate_info.descriptorPool = pool;
    VkDescriptorSet set{VK_NULL_HANDLE};
    if (vkAllocateDescriptorSets(device, &allocate_info, &set) != VK_SUCCESS) return {VK_NULL_HANDLE, VK_NULL_HANDLE};
    return {set, pool};
}

bool Device::Impl::exists(const BindResource& r) {
    switch (r.kind) {
        case BindingKind::sampled_texture:
            return textures.contains(r.view.texture) && samplers.contains(r.sampler);
        case BindingKind::texture:
        case BindingKind::storage_texture:
            return textures.contains(r.view.texture);
        case BindingKind::sampler:
            return samplers.contains(r.sampler);
        case BindingKind::uniform_buffer:
        case BindingKind::storage_buffer:
            return buffers.contains(r.buffer.buffer);
        case BindingKind::acceleration_structure:
            return accelerations.contains(r.structure);
    }
    return false;
}

Result<> Device::Impl::check_resources(const PlacedSet& placed) {
    for (const auto& entry : placed.entries) {
        if (!exists(entry.resource)) {
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
                   shared_layouts.get(shared)->desc.name + "', bound through a group of that layout";
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

Result<SharedLayout> Device::create(const SharedLayoutDesc& desc) {
    Impl& self = *impl_;
    const std::string what = "Shared layout '" + desc.name + "'";
    if (desc.bindings.empty()) return make_error(what + ": no bindings");

    Impl::SharedRecord record;
    record.desc = desc;
    auto& bindings = record.desc.bindings;
    std::ranges::sort(bindings, {}, &ShaderBinding::binding);
    for (size_t i = 0; i < bindings.size(); ++i) {
        ShaderBinding& binding = bindings[i];
        const std::string number = std::to_string(binding.binding);
        if (binding.name.empty()) return make_error(what + ": binding " + number + " has no name");
        if (binding.count == 0) {
            return make_error(what + ": '" + binding.name +
                              "' holds no elements; an array shaders size at run time is given "
                              "the most it holds");
        }
        if (binding.kind == BindingKind::acceleration_structure && !self.caps.ray_queries) {
            return make_error(what + ": '" + binding.name +
                              "' is an acceleration structure, which this GPU has no ray queries for");
        }
        for (size_t j = 0; j < i; ++j) {
            if (bindings[j].binding == binding.binding) {
                return make_error(what + ": binding " + number + " is declared twice");
            }
            if (bindings[j].name == binding.name) {
                return make_error(what + ": two bindings are named '" + binding.name + "'");
            }
        }
        binding.set = desc.usual_set;
        binding.array = binding.array || binding.count > 1;
    }

    std::vector<VkDescriptorSetLayoutBinding> native(bindings.size());
    std::vector<VkDescriptorBindingFlags> flags(bindings.size());
    uint32_t elements = 0;
    for (size_t i = 0; i < bindings.size(); ++i) {
        const ShaderBinding& binding = bindings[i];
        const VkShaderStageFlags stages = vulkan::to_vk(binding.stages);
        native[i].binding = binding.binding;
        native[i].descriptorType = vulkan::to_vk(binding.kind);
        native[i].descriptorCount = binding.count;
        native[i].stageFlags = stages != 0 ? stages : VkShaderStageFlags{VK_SHADER_STAGE_ALL};
        // An array's elements are filled as they come; shaders read only
        // those they are told of.
        if (binding.array) flags[i] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
        record.first.push_back(elements);
        elements += binding.count;
        add_size(record.set_sizes, native[i].descriptorType, binding.count);
    }
    VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info{};
    flags_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flags_info.bindingCount = static_cast<uint32_t>(flags.size());
    flags_info.pBindingFlags = flags.data();
    VkDescriptorSetLayoutCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    create_info.pNext = &flags_info;
    create_info.bindingCount = static_cast<uint32_t>(native.size());
    create_info.pBindings = native.data();
    if (vkCreateDescriptorSetLayout(self.device, &create_info, nullptr, &record.layout) != VK_SUCCESS) {
        return make_error(what + ": the set layout could not be made");
    }
    vulkan::name_object(self.device, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
                        reinterpret_cast<uint64_t>(record.layout), desc.name);
    return self.shared_layouts.emplace(std::move(record));
}

Result<Owned<BindGroup>> Device::create(const SharedGroupDesc& desc) {
    Impl& self = *impl_;
    const std::string what = named("Shared group", desc.name);
    Impl::SharedRecord* shared = self.shared_layouts.get(desc.layout);
    if (shared == nullptr) return make_error(what + ": the shared layout does not exist");

    auto state = std::make_unique<Impl::SharedGroupState>();
    state->contents.resize(shared->first.back() + shared->desc.bindings.back().count);
    state->name = desc.name;
    SharedChanges changes;
    changes.written.set = shared->desc.usual_set;
    if (!desc.entries.empty()) {
        auto placed = place_some(shared->desc, desc.entries);
        if (!placed) return make_error(what + ": " + placed.error());
        auto split = split_changes(self, *shared, *placed);
        if (!split) return make_error(what + ": " + split.error());
        changes = std::move(*split);
        apply_contents(*state, *shared, changes);
    }
    // Only an array's elements may be left for later: a single binding left
    // empty is read by every shader that declares it.
    for (size_t i = 0; i < shared->desc.bindings.size(); ++i) {
        const ShaderBinding& binding = shared->desc.bindings[i];
        if (!binding.array && !state->contents[shared->first[i]]) {
            return make_error(what + ": '" + binding.name + "' is not given");
        }
    }

    Impl::GroupRecord record;
    std::tie(record.set, record.pool) = self.allocate_shared(*shared);
    if (record.set == VK_NULL_HANDLE) return make_error(what + ": no descriptor set could be allocated");
    if (!changes.written.entries.empty()) self.write_set(record.set, changes.written);
    vulkan::name_object(self.device, VK_OBJECT_TYPE_DESCRIPTOR_SET, reinterpret_cast<uint64_t>(record.set),
                        desc.name);
    record.layout = shared->layout;
    record.set_index = shared->desc.usual_set;
    record.shared = desc.layout;
    record.state = std::move(state);
    return Owned<BindGroup>(*this, self.groups.emplace(std::move(record)));
}

Result<> Device::update(BindGroup group, std::span<const BindEntry> entries) {
    Impl& self = *impl_;
    Impl::GroupRecord* record = self.groups.get(group);
    if (record == nullptr) return make_error("Updating a bind group that no longer exists");
    if (record->state) return update_shared(self, *record, entries);
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
    if (!record.has_value()) return;
    self.release_later([dev = self.device, pool = record->pool, set = record->set] {
        vkFreeDescriptorSets(dev, pool, 1, &set);
    });
}

} // namespace fjell::gpu
