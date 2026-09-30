#pragma once

#include "gpu/command_list.hpp"
#include "gpu/device.hpp"
#include "gpu/vulkan/command_list_impl.hpp"
#include "gpu/vulkan/frame_descriptor_cache.hpp"

#include <vulkan/vulkan.h>

// The Vulkan objects behind the interface's handles, for the backend's own
// code and for its tests and benchmarks (tests/*_vulkan_*.cpp). Nothing
// outside the backend reaches them: cmake/GpuBackendCheck.cmake sees to it.

namespace fjell::gpu::vulkan {

/// The VkBuffer behind a buffer; null when the handle finds none.
[[nodiscard]] VkBuffer native_buffer(Device& device, Buffer buffer);

/// The VkAccelerationStructureKHR behind an acceleration structure; null
/// when the handle finds none.
[[nodiscard]] VkAccelerationStructureKHR native_acceleration(Device& device, AccelerationStructure structure);

/// The VkImageView for a view, made the first time it is asked for and kept
/// with the texture; null when the handle finds no texture.
[[nodiscard]] VkImageView native_view(Device& device, const TextureView& view);

/// The VkSampler behind a sampler; null when the handle finds none.
[[nodiscard]] VkSampler native_sampler(Device& device, Sampler sampler);

/// A handle to an image made outside the device (a swapchain's), described by
/// `info`, with
/// `whole_view` (which may be null) as the view of all of it. The device
/// never destroys the image or that view: releasing the handle forgets it and
/// destroys only the views asked of it since. The image's maker keeps it alive
/// at least as long as the handle.
[[nodiscard]] Owned<Texture> adopt(Device& device, VkImage image, VkImageView whole_view,
                                   const TextureInfo& info);

/// The VkPipeline behind a pipeline; null when the handle finds none.
[[nodiscard]] VkPipeline native_pipeline(Device& device, ComputePipeline pipeline);
[[nodiscard]] VkPipeline native_pipeline(Device& device, GraphicsPipeline pipeline);

/// The VkPipelineLayout a pipeline binds with; null when the handle finds none.
[[nodiscard]] VkPipelineLayout native_layout(Device& device, ComputePipeline pipeline);
[[nodiscard]] VkPipelineLayout native_layout(Device& device, GraphicsPipeline pipeline);

/// The VkDescriptorSet behind a bind group; null when the handle finds none.
/// For a shared group the device made, taking the set counts as binding it in
/// the frame recording: an update writes a new version rather than a set that
/// frame may read.
[[nodiscard]] VkDescriptorSet native_group(Device& device, BindGroup group);

/// The command buffer a list records into, for backend code that records
/// Vulkan itself.
[[nodiscard]] VkCommandBuffer native_command_buffer(CommandList& list);

/// The device's one-frame descriptor sets.
[[nodiscard]] FrameDescriptorCache& frame_cache(Device& device);
[[nodiscard]] const FrameDescriptorCache& frame_cache(const Device& device);

/// Reads back the times of the zones that finished, once a frame into a
/// command buffer on the graphics queue. Nothing in builds without the
/// profiler.
void collect_zones(Device& device, VkCommandBuffer cb);

/// A command list recording into a command buffer the backend holds: what it
/// records lands in order with what is recorded into `cb` directly. Each
/// starts with nothing bound.
class CommandBufferList {
public:
    CommandBufferList(Device& device, VkCommandBuffer cb, Queue queue = Queue::graphics) noexcept
        : impl_{.cb = cb, .queue = queue}, list_(device, impl_) {}

    CommandBufferList(const CommandBufferList&) = delete;
    CommandBufferList& operator=(const CommandBufferList&) = delete;

    [[nodiscard]] CommandList& list() noexcept { return list_; }

private:
    CommandList::Impl impl_;
    CommandList list_;
};

} // namespace fjell::gpu::vulkan
