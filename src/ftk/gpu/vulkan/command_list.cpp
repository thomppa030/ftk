#include "ftk/gpu/vulkan/command_list_impl.hpp"

#include "ftk/gpu/vulkan/translate.hpp"
#include "ftk/gpu/vulkan/vk_check.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <string>
#include <vector>

namespace ftk::gpu {

namespace {

using PipelineRecord = Device::Impl::PipelineRecord;

// A descriptor set and the set of the pipeline it goes to.
struct SetAt {
    uint32_t set{0};
    VkDescriptorSet native{VK_NULL_HANDLE};
};

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
        // Its set is now read by the frame recording; an update writes a
        // new version until that frame is finished.
        if (record->state) record->state->bound.store(device.recording, std::memory_order_relaxed);
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

    SmallVector<FrameCacheBinding, INLINE_SET_ENTRIES> described;
    for (const PlacedEntry& entry : placed->entries) described.push_back(device.describe(entry));

    const VkDescriptorSet native =
        device.frame_sets.acquire(pipeline.layout.set_layouts[placed->set], described);
    if (native == VK_NULL_HANDLE) return make_error("no descriptor set could be had this frame");
    return SetAt{placed->set, native};
}

// Binds a group or frame set where it goes, or refuses it.
void bind_at(Device& device, CommandList::Impl& self, const Result<SetAt>& at) {
    if (!at) {
        vulkan::refuse(device, self, at.error());
        return;
    }
    vkCmdBindDescriptorSets(self.cb, self.pipeline->bind_point, self.pipeline->layout.layout,
                            at->set, 1, &at->native, 0, nullptr);
    self.bound_sets |= 1U << at->set;
}

// Whether a pipeline is set, refusing `what` when none is.
bool has_pipeline(Device& device, CommandList::Impl& self, const char* what) {
    if (self.pipeline != nullptr) return true;
    vulkan::refuse(device, self, std::string(what) + " before a pipeline is set");
    return false;
}

} // namespace

// ── Shared with the render encoder ──────────────────────────────────────

void vulkan::refuse(Device& device, CommandList::Impl& list, const std::string& why) {
    const std::string who =
        list.pipeline != nullptr ? list.pipeline->name : std::string("A command list");
    device.impl().report_once(who + ": " + why);
    list.refused = true;
}

bool vulkan::outside_render(Device& device, CommandList::Impl& list, const char* what) {
    if (!list.rendering) return true;
    device.impl().report_once(std::string("A command list ") + what +
                              " while its render scope is open; the encoder records until it ends");
    return false;
}

void vulkan::use_pipeline(Device& device, CommandList::Impl& list,
                          const Device::Impl::PipelineRecord* pipeline) {
    list.pipeline = pipeline;
    list.bound_sets = 0;
    list.refused = false;
    if (pipeline == nullptr) {
        refuse(device, list, "sets a pipeline that no longer exists");
        return;
    }
    vkCmdBindPipeline(list.cb, pipeline->bind_point, pipeline->pipeline);
}

void vulkan::bind_group(Device& device, CommandList::Impl& list, BindGroup group) {
    if (!has_pipeline(device, list, "binds a group")) return;
    bind_at(device, list, group_at(device.impl(), *list.pipeline, group));
}

void vulkan::bind_entries(Device& device, CommandList::Impl& list,
                          std::span<const BindEntry> entries) {
    if (!has_pipeline(device, list, "binds resources")) return;
    bind_at(device, list, frame_set(device.impl(), *list.pipeline, entries));
}

void vulkan::push(Device& device, CommandList::Impl& list, std::span<const std::byte> bytes) {
    if (!has_pipeline(device, list, "pushes data")) return;
    const auto size = push_size(list.pipeline->shader_layout, bytes.size());
    if (!size) {
        refuse(device, list, size.error());
        return;
    }
    vkCmdPushConstants(list.cb, list.pipeline->layout.layout,
                       to_vk(list.pipeline->shader_layout.push_stages), 0, *size, bytes.data());
}

bool vulkan::ready(Device& device, CommandList::Impl& list, const char* what) {
    if (!has_pipeline(device, list, what) || list.refused) return false;
    const uint32_t missing = list.pipeline->declared_sets & ~list.bound_sets;
    if (missing != 0) {
        refuse(device, list,
               "set " + std::to_string(std::countr_zero(missing)) +
                   " is declared by its shaders and nothing is bound there");
        return false;
    }
    return true;
}

// ── CommandList ─────────────────────────────────────────────────────────

void CommandList::set_pipeline(ComputePipeline pipeline) {
    if (!vulkan::outside_render(*device_, *impl_, "sets a compute pipeline")) return;
    vulkan::use_pipeline(*device_, *impl_, device_->impl().compute_pipelines.get(pipeline));
}

void CommandList::bind(BindGroup group) {
    if (!vulkan::outside_render(*device_, *impl_, "binds a group")) return;
    vulkan::bind_group(*device_, *impl_, group);
}

void CommandList::bind(std::span<const BindEntry> entries) {
    if (!vulkan::outside_render(*device_, *impl_, "binds resources")) return;
    vulkan::bind_entries(*device_, *impl_, entries);
}

void CommandList::push_bytes(std::span<const std::byte> bytes) {
    if (!vulkan::outside_render(*device_, *impl_, "pushes data")) return;
    vulkan::push(*device_, *impl_, bytes);
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
    if (!vulkan::outside_render(*device_, *impl_, "dispatches")) return;
    if (!vulkan::ready(*device_, *impl_, "dispatches")) return;
    vkCmdDispatch(impl_->cb, x, y, z);
}

void CommandList::dispatch_indirect(BufferRange args) {
    if (!vulkan::outside_render(*device_, *impl_, "dispatches")) return;
    if (!vulkan::ready(*device_, *impl_, "dispatches")) return;
    const auto* buffer = device_->impl().buffers.get(args.buffer);
    if (buffer == nullptr) {
        vulkan::refuse(*device_, *impl_,
                       "dispatches from arguments in a buffer that no longer exists");
        return;
    }
    vkCmdDispatchIndirect(impl_->cb, buffer->buffer, args.offset);
}

void CommandList::execute(std::span<CommandList* const> recorded_in_parallel) {
    if (!vulkan::outside_render(*device_, *impl_, "plays other lists")) return;
    std::vector<VkCommandBuffer> recorded;
    recorded.reserve(recorded_in_parallel.size());
    for (CommandList* list : recorded_in_parallel) {
        if (list->impl_->rendering) {
            device_->impl().report_once("A command list plays one whose render scope is open");
            return;
        }
        if (list->impl_->parallel_open) {
            vk_check(vkEndCommandBuffer(list->impl_->cb), "Failed to end a list recorded on another thread");
            list->impl_->parallel_open = false;
        }
        recorded.push_back(list->impl_->cb);
    }
    if (recorded.empty()) return;
    vkCmdExecuteCommands(impl_->cb, static_cast<uint32_t>(recorded.size()), recorded.data());
}

} // namespace ftk::gpu
