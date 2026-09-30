#include "gpu/vulkan/frame_impl.hpp"

#include "core/small_vector.hpp"
#include "gpu/vulkan/device_impl.hpp"
#include "renderer/gpu/frames_in_flight.hpp"
#include "renderer/gpu/gpu_core.hpp"
#include "renderer/gpu/upload_context.hpp"
#include "renderer/gpu/vk_check.hpp"

#include <algorithm>
#include <limits>
#include <span>
#include <string>

// Frames on the Vulkan device: a command pool per queue in each slot, reset
// when the slot comes round, and the frame's lists submitted when it ends,
// each on its own batch, chained across the queues by the queue timelines.

namespace fjell::gpu {

namespace {

// The index of `queue` in a frame's per-queue arrays.
size_t queue_index(Queue queue) {
    return queue == Queue::compute ? 1 : 0;
}

VkSemaphoreSubmitInfo semaphore_at(VkSemaphore semaphore, uint64_t value,
                                   VkPipelineStageFlags2 stages) {
    VkSemaphoreSubmitInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    info.semaphore = semaphore;
    info.value = value;
    info.stageMask = stages;
    return info;
}

// Makes the pool for `family` that a frame's command buffers come from.
VkCommandPool make_pool(VkDevice device, uint32_t family) {
    VkCommandPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    info.queueFamilyIndex = family;
    VkCommandPool pool{VK_NULL_HANDLE};
    vk_check(vkCreateCommandPool(device, &info, nullptr, &pool), "Failed to create a frame's command pool");
    return pool;
}

} // namespace

// ── Frame ───────────────────────────────────────────────────────────────

uint64_t Frame::serial() const noexcept {
    return impl_->serial;
}

uint32_t Frame::slot() const noexcept {
    return impl_->slot;
}

CommandList& Frame::commands(Queue queue) {
    Device::Impl& device = device_->impl();
    if (queue == Queue::compute && !device.caps.async_compute) {
        device.report_once("A frame asks for a compute list on a device without a compute queue; "
                           "it records for the graphics queue");
        queue = Queue::graphics;
    }
    const size_t q = queue_index(queue);
    if (impl_->used[q] == impl_->buffers[q].size()) {
        VkCommandBufferAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate.commandPool = impl_->pools[q];
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        VkCommandBuffer made{VK_NULL_HANDLE};
        vk_check(vkAllocateCommandBuffers(device.device, &allocate, &made),
                 "Failed to allocate a frame's command buffer");
        vulkan::name_object(device.device, VK_OBJECT_TYPE_COMMAND_BUFFER,
                            reinterpret_cast<uint64_t>(made),
                            std::string(q == 0 ? "graphics" : "compute") + " list " +
                                std::to_string(impl_->buffers[q].size()) + " of frame slot " +
                                std::to_string(impl_->slot));
        impl_->buffers[q].push_back(made);
    }
    VkCommandBuffer cb = impl_->buffers[q][impl_->used[q]++];
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(cb, &begin), "Failed to begin a frame's command buffer");
    impl_->lists.push_back(std::make_unique<FrameList>(*device_, cb, queue));
    return impl_->lists.back()->list;
}

void Frame::submit(CommandList& list, const SubmitDesc& desc) {
    Device::Impl& device = device_->impl();
    const bool ours = std::ranges::any_of(
        impl_->lists, [&](const auto& handed) { return &handed->list == &list; });
    if (!ours) {
        device.report_once("A frame is submitted a list it did not hand out");
        return;
    }
    const auto submitted = [&](const CommandList* which) {
        return std::ranges::any_of(impl_->submissions,
                                   [&](const auto& s) { return s.list == which; });
    };
    if (submitted(&list)) {
        device.report_once("A frame is submitted the same list twice");
        return;
    }
    if (desc.after != nullptr && !submitted(desc.after)) {
        device.report_once("A frame's list waits for one not submitted before it");
        return;
    }
    impl_->submissions.push_back({&list, desc.after});
}

// ── Device ──────────────────────────────────────────────────────────────

Frame& Device::begin_frame() {
    Impl& self = *impl_;
    const uint64_t serial = ++self.frame_serial;
    const auto count = static_cast<uint64_t>(self.frames.size());
    const auto slot = static_cast<uint32_t>((serial - 1) % count);

    // The frame that last used this slot has to be done before its command
    // buffers and one-frame memory are used again.
    if (serial > count) {
        const uint64_t done = serial - count;
        VkSemaphoreWaitInfo wait{};
        wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wait.semaphoreCount = 1;
        wait.pSemaphores = &self.frame_timeline;
        wait.pValues = &done;
        vk_check(vkWaitSemaphores(self.device, &wait, std::numeric_limits<uint64_t>::max()),
                 "Failed to wait for a frame to finish");
    }

    if (!self.frames[slot]) {
        self.frames[slot] = std::make_unique<FrameSlot>(*this);
        for (size_t q = 0; q < Frame::Impl::QUEUES; ++q) {
            if (self.queues[q] != VK_NULL_HANDLE) {
                self.frames[slot]->impl.pools[q] = make_pool(self.device, self.families[q]);
            }
        }
    }
    Frame::Impl& frame = self.frames[slot]->impl;
    if (frame.open) self.report_once("A frame began while the one before in its slot never ended");
    for (VkCommandPool pool : frame.pools) {
        if (pool != VK_NULL_HANDLE) {
            vk_check(vkResetCommandPool(self.device, pool, 0), "Failed to reset a frame's command pool");
        }
    }
    frame.used = {};
    frame.lists.clear();
    frame.submissions.clear();
    frame.acquired = VK_NULL_HANDLE;
    frame.rendered = VK_NULL_HANDLE;
    frame.serial = serial;
    frame.slot = slot;
    frame.open = true;

    self.frame_slot = slot;
    self.frame_sets.begin_frame(slot);
    self.transient.begin_frame(slot);
    self.scratch.begin_frame(slot);
    self.recording = serial;
    self.releases.collect(finished_frame());
    return self.frames[slot]->frame;
}

Result<> Device::end_frame(Frame& frame) {
    Impl& self = *impl_;
    Frame::Impl& f = frame.impl();
    f.open = false;

    // Uploads recorded since the last frame go first; the frame's first list
    // waits for them.
    UploadContext& upload = self.core.upload_context();
    upload.flush();

    const auto submit = [&](VkQueue queue, std::span<const VkSemaphoreSubmitInfo> waits,
                            VkCommandBuffer cb,
                            std::span<const VkSemaphoreSubmitInfo> signals) -> Result<> {
        VkCommandBufferSubmitInfo cb_info{};
        cb_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cb_info.commandBuffer = cb;
        VkSubmitInfo2 info{};
        info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        info.waitSemaphoreInfoCount = static_cast<uint32_t>(waits.size());
        info.pWaitSemaphoreInfos = waits.data();
        info.commandBufferInfoCount = cb != VK_NULL_HANDLE ? 1 : 0;
        info.pCommandBufferInfos = &cb_info;
        info.signalSemaphoreInfoCount = static_cast<uint32_t>(signals.size());
        info.pSignalSemaphoreInfos = signals.data();
        const VkResult result = vkQueueSubmit2(queue, 1, &info, VK_NULL_HANDLE);
        if (result == VK_SUCCESS) return {};
        if (result == VK_ERROR_DEVICE_LOST) self.core.device().dump_device_fault("frame submit");
        return make_error("A frame's submission failed (VkResult=" +
                          std::to_string(static_cast<int>(result)) + ")");
    };

    // The frame is finished when its last list is, on the graphics queue:
    // frames end in order there, so the frame timeline only climbs.
    const auto finishing = [&](SmallVector<VkSemaphoreSubmitInfo, 4>& signals) {
        if (f.rendered != VK_NULL_HANDLE) {
            signals.push_back(semaphore_at(f.rendered, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT));
        }
        signals.push_back(
            semaphore_at(self.frame_timeline, f.serial, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT));
    };

    const VkSemaphoreSubmitInfo upload_wait = semaphore_at(
        upload.buffer_timeline(), upload.last_buffer_value(), VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    SmallVector<uint64_t, 4> reached;
    bool acquire_waited = false;
    for (size_t i = 0; i < f.submissions.size(); ++i) {
        const auto& submission = f.submissions[i];
        CommandList::Impl& list = submission.list->impl();
        if (list.rendering) {
            self.report_once("A frame's list is submitted with its render scope still open");
        }
        vk_check(vkEndCommandBuffer(list.cb), "Failed to end a frame's command buffer");

        SmallVector<VkSemaphoreSubmitInfo, 4> waits;
        if (i == 0) waits.push_back(upload_wait);
        if (list.queue == Queue::graphics && !acquire_waited && f.acquired != VK_NULL_HANDLE) {
            // The swapchain image is written at colour output, after the
            // acquire; everything before that stage may run first.
            waits.push_back(
                semaphore_at(f.acquired, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT));
            acquire_waited = true;
        }
        if (submission.after != nullptr) {
            for (size_t j = 0; j < i; ++j) {
                if (f.submissions[j].list != submission.after) continue;
                const size_t other = queue_index(f.submissions[j].list->impl().queue);
                waits.push_back(semaphore_at(self.queue_timelines[other], reached[j],
                                             VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT));
            }
        }

        const size_t q = queue_index(list.queue);
        const uint64_t value = ++self.queue_values[q];
        reached.push_back(value);
        SmallVector<VkSemaphoreSubmitInfo, 4> signals;
        signals.push_back(
            semaphore_at(self.queue_timelines[q], value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT));
        const bool last = i + 1 == f.submissions.size();
        if (last && list.queue == Queue::graphics) finishing(signals);

        if (auto sent = submit(self.queues[q], waits, list.cb, signals); !sent) return sent;
    }

    // Nothing submitted, or the last list on the compute queue: an empty
    // batch on the graphics queue, after the last list, finishes the frame.
    const bool finished_by_list =
        !f.submissions.empty() && f.submissions.back().list->impl().queue == Queue::graphics;
    if (!finished_by_list) {
        SmallVector<VkSemaphoreSubmitInfo, 4> waits;
        if (!f.submissions.empty()) {
            waits.push_back(semaphore_at(self.queue_timelines[1], reached.back(),
                                         VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT));
        }
        // A swapchain image no list drew still has to be acquired before
        // the frame signals it rendered.
        if (!acquire_waited && f.acquired != VK_NULL_HANDLE) {
            waits.push_back(semaphore_at(f.acquired, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT));
        }
        SmallVector<VkSemaphoreSubmitInfo, 4> signals;
        finishing(signals);
        if (auto sent = submit(self.queues[0], waits, VK_NULL_HANDLE, signals); !sent) return sent;
    }
    return {};
}

uint64_t Device::finished_frame() const {
    uint64_t value = 0;
    vk_check(vkGetSemaphoreCounterValue(impl_->device, impl_->frame_timeline, &value),
             "Failed to read the frame timeline");
    return value;
}

} // namespace fjell::gpu
