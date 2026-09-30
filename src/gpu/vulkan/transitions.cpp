#include "gpu/transition.hpp"

#include "core/log.hpp"
#include "core/small_vector.hpp"
#include "gpu/vulkan/access.hpp"
#include "gpu/vulkan/command_list_impl.hpp"
#include "gpu/vulkan/native.hpp"

#include <cstdint>
#include <cstdlib>
#include <format>

// The frame graph's transitions as Vulkan pipeline barriers, and the two
// questions the graph asks while it works them out.

namespace fjell::gpu {

namespace {


// Barriers a pass needs between it and the one before; more go to the heap.
constexpr size_t INLINE_BARRIERS = 16;

// `stages` as a barrier on `queue` may name them.
VkPipelineStageFlags2 on_queue(VkPipelineStageFlags2 stages, Queue queue) {
    return queue == Queue::compute ? vulkan::compute_queue_stages(stages) : stages;
}

const char* queue_name(Queue queue) {
    return queue == Queue::compute ? "compute" : "graphics";
}

// FJELL_TRACE_LAYOUT: every barrier logged with its image or buffer's handle,
// to match against the handles validation messages name.
bool layout_trace_enabled() {
    static const bool on = [] {
        const char* value = std::getenv("FJELL_TRACE_LAYOUT");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return on;
}

// The barrier a texture transition becomes, or none. Work waited on from the
// other queue is ordered by the submission between the queues; the barrier
// is then needed only for a change of layout, and its source scope has to
// reach that submission's wait for the change to be ordered after it: every
// stage covers whichever stage the wait is at, and the submission has
// already made the memory available, so no access is named.
std::optional<VkImageMemoryBarrier2> image_barrier(const Device::Impl::TextureRecord& record,
                                                   const Transition& t, Queue queue) {
    const bool depth = (record.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
    const vulkan::ImageScope from = vulkan::merged_image_scope(t.from, depth);
    const vulkan::ImageScope to = vulkan::merged_image_scope(t.to, depth);
    const vulkan::ImageScope next = vulkan::merged_image_scope(t.visible_to, depth);
    const VkPipelineStageFlags2 src = vulkan::merged_image_scope(t.wait_for, depth).stages;
    const bool layout_change = from.layout != to.layout;
    const bool cross_queue = on_queue(src, queue) != src;
    if (!layout_change && (cross_queue || src == 0)) return std::nullopt;

    const ResolvedView range = resolve(t.texture, record.info);
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = cross_queue ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
                         : src == 0    ? VK_PIPELINE_STAGE_2_NONE
                                       : src;
    barrier.srcAccessMask =
        cross_queue ? 0 : vulkan::merged_image_scope(t.flush, depth).access & vulkan::WRITE_ACCESS_BITS;
    barrier.dstStageMask = on_queue(next.stages, queue);
    barrier.dstAccessMask = next.access;
    barrier.oldLayout = from.layout;
    barrier.newLayout = to.layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = record.image;
    barrier.subresourceRange = {record.aspect, range.base_mip, range.mip_count, range.base_layer,
                                range.layer_count};
    return barrier;
}

// The barrier a buffer transition becomes, or none. A buffer has no layout,
// so work waited on from the other queue leaves it nothing to carry.
std::optional<VkBufferMemoryBarrier2> buffer_barrier(VkBuffer buffer, const Transition& t,
                                                     Queue queue) {
    const VkPipelineStageFlags2 src = vulkan::buffer_scope(t.wait_for).stages;
    if (src == 0 || on_queue(src, queue) != src) return std::nullopt;
    const vulkan::BufferScope next = vulkan::buffer_scope(t.visible_to);

    VkBufferMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    barrier.srcStageMask = src;
    barrier.srcAccessMask = vulkan::buffer_scope(t.flush).access & vulkan::WRITE_ACCESS_BITS;
    barrier.dstStageMask = on_queue(next.stages, queue);
    barrier.dstAccessMask = next.access;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffer;
    barrier.offset = 0;
    barrier.size = VK_WHOLE_SIZE;
    return barrier;
}

// A buffer transition as the layout trace names it: whether the next use
// writes, and whether a write came before it.
const char* buffer_hazard(const Transition& t) {
    bool writes = false;
    t.visible_to.for_each([&](Access access) { writes = writes || access_is_write(access); });
    if (!writes) return "RAW";
    return t.flush.empty() ? "WAR" : "WAW/WAR";
}

} // namespace

bool same_state(AccessSet a, AccessSet b, bool depth) {
    return vulkan::merged_image_scope(a, depth).layout == vulkan::merged_image_scope(b, depth).layout;
}

bool texture_already_visible(AccessSet made_visible, AccessSet wanted, bool depth, Queue queue) {
    const vulkan::ImageScope made = vulkan::merged_image_scope(made_visible, depth);
    const vulkan::ImageScope want = vulkan::merged_image_scope(wanted, depth);
    return (on_queue(want.stages, queue) & ~on_queue(made.stages, queue)) == 0 &&
           (want.access & ~made.access) == 0;
}

bool buffer_already_visible(AccessSet made_visible, AccessSet wanted, Queue queue) {
    const vulkan::BufferScope made = vulkan::buffer_scope(made_visible);
    const vulkan::BufferScope want = vulkan::buffer_scope(wanted);
    return (on_queue(want.stages, queue) & ~on_queue(made.stages, queue)) == 0 &&
           (want.access & ~made.access) == 0;
}

void CommandList::transition(std::span<const Transition> transitions, std::string* trace) {
    if (!vulkan::outside_render(*device_, *impl_, "records transitions")) return;
    Device::Impl& device = device_->impl();
    const Queue queue = impl_->queue;

    SmallVector<VkImageMemoryBarrier2, INLINE_BARRIERS> images;
    SmallVector<VkBufferMemoryBarrier2, INLINE_BARRIERS> buffers;
    // Acceleration structures are memory reached through their addresses:
    // a memory barrier each, which the pipeline barrier merges.
    SmallVector<VkMemoryBarrier2, INLINE_BARRIERS> structures;
    for (const Transition& t : transitions) {
        if (t.structure.valid()) {
            if (!device.accelerations.contains(t.structure)) {
                device.report_once("A transition names an acceleration structure that no longer exists");
                continue;
            }
            // A buffer's barrier asks what a structure's does: the same
            // stages, accesses and queue.
            const auto barrier = buffer_barrier(VK_NULL_HANDLE, t, queue);
            if (!barrier) continue;
            VkMemoryBarrier2 memory{};
            memory.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
            memory.srcStageMask = barrier->srcStageMask;
            memory.srcAccessMask = barrier->srcAccessMask;
            memory.dstStageMask = barrier->dstStageMask;
            memory.dstAccessMask = barrier->dstAccessMask;
            structures.push_back(memory);
            if (trace != nullptr) {
                *trace += std::format("    structure {} src {:x}/{:x} dst {:x}/{:x}\n", t.name,
                                      memory.srcStageMask, memory.srcAccessMask,
                                      memory.dstStageMask, memory.dstAccessMask);
            }
            continue;
        }
        if (t.texture.texture.valid()) {
            const auto* record = device.textures.get(t.texture.texture);
            if (record == nullptr) {
                device.report_once("A transition names a texture that no longer exists");
                continue;
            }
            const auto barrier = image_barrier(*record, t, queue);
            if (!barrier) continue;
            images.push_back(*barrier);
            const VkImageSubresourceRange& range = barrier->subresourceRange;
            if (trace != nullptr) {
                *trace += std::format(
                    "    image {} mips {}+{} layers {}+{} {} -> {} src {:x}/{:x} dst {:x}/{:x}\n",
                    t.name, range.baseMipLevel, range.levelCount, range.baseArrayLayer,
                    range.layerCount, vulkan::layout_name(barrier->oldLayout),
                    vulkan::layout_name(barrier->newLayout), barrier->srcStageMask,
                    barrier->srcAccessMask, barrier->dstStageMask, barrier->dstAccessMask);
            }
            if (layout_trace_enabled()) {
                FJELL_GFX_INFO(
                    "[layout] graph barrier img=0x{:x} '{}' {} -> {} src=0x{:x} dst=0x{:x} on {} "
                    "cb=0x{:x}",
                    reinterpret_cast<uintptr_t>(barrier->image), t.name,
                    vulkan::layout_name(barrier->oldLayout), vulkan::layout_name(barrier->newLayout),
                    static_cast<uint64_t>(barrier->srcStageMask),
                    static_cast<uint64_t>(barrier->dstStageMask), queue_name(queue),
                    reinterpret_cast<uintptr_t>(impl_->cb));
            }
            continue;
        }
        const auto* record = device.buffers.get(t.buffer);
        if (record == nullptr) {
            device.report_once("A transition names a buffer that no longer exists");
            continue;
        }
        const auto barrier = buffer_barrier(record->buffer, t, queue);
        if (!barrier) continue;
        buffers.push_back(*barrier);
        if (trace != nullptr) {
            *trace += std::format("    buffer {} src {:x}/{:x} dst {:x}/{:x}\n", t.name,
                                  barrier->srcStageMask, barrier->srcAccessMask,
                                  barrier->dstStageMask, barrier->dstAccessMask);
        }
        if (layout_trace_enabled()) {
            FJELL_GFX_INFO(
                "[layout] buffer barrier buf=0x{:x} '{}' {} src=0x{:x}/0x{:x} dst=0x{:x}/0x{:x} on {}",
                reinterpret_cast<uintptr_t>(barrier->buffer), t.name, buffer_hazard(t),
                static_cast<uint64_t>(barrier->srcStageMask),
                static_cast<uint64_t>(barrier->srcAccessMask),
                static_cast<uint64_t>(barrier->dstStageMask),
                static_cast<uint64_t>(barrier->dstAccessMask), queue_name(queue));
        }
    }
    if (images.empty() && buffers.empty() && structures.empty()) return;

    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = static_cast<uint32_t>(structures.size());
    dependency.pMemoryBarriers = structures.data();
    dependency.imageMemoryBarrierCount = static_cast<uint32_t>(images.size());
    dependency.pImageMemoryBarriers = images.data();
    dependency.bufferMemoryBarrierCount = static_cast<uint32_t>(buffers.size());
    dependency.pBufferMemoryBarriers = buffers.data();
    vkCmdPipelineBarrier2(impl_->cb, &dependency);
}

} // namespace fjell::gpu
