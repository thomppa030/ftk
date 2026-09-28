#include "gpu/vulkan/command_list_impl.hpp"

#include "gpu/vulkan/translate.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <string>
#include <vector>

namespace fjell::gpu {

namespace {

using PipelineRecord = Device::Impl::PipelineRecord;

// A descriptor set and the set of the pipeline it goes to.
struct SetAt {
    uint32_t set{0};
    VkDescriptorSet native{VK_NULL_HANDLE};
};

// Reports why the list refuses what it was asked, naming the pipeline, and
// holds back its dispatches until the next pipeline.
void refuse(Device& device, CommandList::Impl& self, const std::string& why) {
    const std::string who =
        self.pipeline != nullptr ? self.pipeline->name : std::string("A command list");
    device.impl().report_once(who + ": " + why);
    self.refused = true;
}

// Where a group goes for `pipeline`: a persistent group at the set it was
// made for, a shared one at the set the pipeline's shaders declare it at.
Result<SetAt> group_at(Device::Impl& device, const PipelineRecord& pipeline, BindGroup group) {
    const Device::Impl::GroupRecord* record = device.groups.get(group);
    if (record == nullptr) return make_error("binds a group that no longer exists");

    if (record->shared.valid()) {
        const auto& shared_sets = pipeline.layout.shared_sets;
        const auto taken =
            std::ranges::find(shared_sets, record->shared, [](const auto& s) { return s.first; });
        if (taken == shared_sets.end()) {
            return make_error("binds the shared '" +
                              device.shared_layouts.get(record->shared)->desc.name +
                              "', which the pipeline does not name");
        }
        return SetAt{taken->second, record->set};
    }
    const auto& layouts = pipeline.layout.set_layouts;
    if (record->set_index >= layouts.size() || layouts[record->set_index] != record->layout) {
        return make_error("binds a group made for set " + std::to_string(record->set_index) +
                          " of shaders that declare it differently");
    }
    return SetAt{record->set_index, record->set};
}

// A set holding `entries` for this frame, shared with any other bind of the
// same resources to the same layout this frame.
Result<SetAt> frame_set(Device::Impl& device, const PipelineRecord& pipeline,
                        std::span<const BindEntry> entries) {
    auto placed = place(pipeline.shader_layout, entries);
    if (!placed) return std::unexpected(placed.error());
    if (std::string shared = device.shared_at(pipeline, placed->set); !shared.empty()) {
        return make_error(std::move(shared));
    }
    if (auto present = device.check_resources(*placed); !present) {
        return std::unexpected(present.error());
    }

    std::vector<FrameCacheBinding> described;
    described.reserve(placed->entries.size());
    for (const PlacedEntry& entry : placed->entries) described.push_back(device.describe(entry));

    const VkDescriptorSet native =
        device.frame_sets.acquire(pipeline.layout.set_layouts[placed->set], described);
    if (native == VK_NULL_HANDLE) return make_error("no descriptor set could be had this frame");
    return SetAt{placed->set, native};
}

// Binds a group or frame set where it goes, or refuses it.
void bind_at(Device& device, CommandList::Impl& self, const Result<SetAt>& at) {
    if (!at) {
        refuse(device, self, at.error());
        return;
    }
    vkCmdBindDescriptorSets(self.cb, self.pipeline->bind_point, self.pipeline->layout.layout,
                            at->set, 1, &at->native, 0, nullptr);
    self.bound_sets |= 1U << at->set;
}

// Whether a pipeline is set, refusing `what` when none is.
bool has_pipeline(Device& device, CommandList::Impl& self, const char* what) {
    if (self.pipeline != nullptr) return true;
    refuse(device, self, std::string(what) + " before a pipeline is set");
    return false;
}

// Whether the pipeline may run: set, nothing refused, every set its shaders
// declare bound.
bool ready(Device& device, CommandList::Impl& self) {
    if (!has_pipeline(device, self, "dispatches") || self.refused) return false;
    const uint32_t missing = self.pipeline->declared_sets & ~self.bound_sets;
    if (missing != 0) {
        refuse(device, self,
               "set " + std::to_string(std::countr_zero(missing)) +
                   " is declared by its shaders and nothing is bound there");
        return false;
    }
    return true;
}

} // namespace

void CommandList::set_pipeline(ComputePipeline pipeline) {
    Impl& self = *impl_;
    self.pipeline = device_->impl().compute_pipelines.get(pipeline);
    self.bound_sets = 0;
    self.refused = false;
    if (self.pipeline == nullptr) {
        refuse(*device_, self, "sets a compute pipeline that no longer exists");
        return;
    }
    vkCmdBindPipeline(self.cb, VK_PIPELINE_BIND_POINT_COMPUTE, self.pipeline->pipeline);
}

void CommandList::bind(BindGroup group) {
    Impl& self = *impl_;
    if (!has_pipeline(*device_, self, "binds a group")) return;
    bind_at(*device_, self, group_at(device_->impl(), *self.pipeline, group));
}

void CommandList::bind(std::span<const BindEntry> entries) {
    Impl& self = *impl_;
    if (!has_pipeline(*device_, self, "binds resources")) return;
    bind_at(*device_, self, frame_set(device_->impl(), *self.pipeline, entries));
}

void CommandList::push_bytes(std::span<const std::byte> bytes) {
    Impl& self = *impl_;
    if (!has_pipeline(*device_, self, "pushes data")) return;
    const auto size = push_size(self.pipeline->shader_layout, bytes.size());
    if (!size) {
        refuse(*device_, self, size.error());
        return;
    }
    vkCmdPushConstants(self.cb, self.pipeline->layout.layout,
                       vulkan::to_vk(self.pipeline->shader_layout.push_stages), 0, *size,
                       bytes.data());
}

BufferRange CommandList::transient_bytes(std::span<const std::byte> bytes) {
    auto slice = device_->impl().transient.allocate(bytes.size());
    if (!slice) {
        device_->impl().report_once(slice.error());
        return {};
    }
    std::memcpy(slice->bytes.data(), bytes.data(), bytes.size());
    return slice->range;
}

void CommandList::dispatch(uint32_t x, uint32_t y, uint32_t z) {
    Impl& self = *impl_;
    if (!ready(*device_, self)) return;
    vkCmdDispatch(self.cb, x, y, z);
}

void CommandList::dispatch_indirect(BufferRange args) {
    Impl& self = *impl_;
    if (!ready(*device_, self)) return;
    const auto* buffer = device_->impl().buffers.get(args.buffer);
    if (buffer == nullptr) {
        refuse(*device_, self, "dispatches from arguments in a buffer that no longer exists");
        return;
    }
    vkCmdDispatchIndirect(self.cb, buffer->buffer, args.offset);
}

} // namespace fjell::gpu
