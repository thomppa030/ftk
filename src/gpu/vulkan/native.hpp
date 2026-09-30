#pragma once

#include "gpu/command_list.hpp"
#include "gpu/device.hpp"
#include "gpu/vulkan/command_list_impl.hpp"
#include "gpu/vulkan/frame_descriptor_cache.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <utility>

// The bridge between the GPU interface and code that still speaks Vulkan,
// both ways, so either can use what the other made while files move onto the
// interface one at a time. It goes when the last file has moved.

namespace fjell::gpu::vulkan {

/// The VkFormat for a format, for Vulkan made outside the interface (pipelines,
/// images) that shares a format with what is made through it.
[[nodiscard]] VkFormat native_format(Format format);

/// The VkBuffer behind a buffer; null when the handle finds none.
[[nodiscard]] VkBuffer native_buffer(Device& device, Buffer buffer);

/// The VkAccelerationStructureKHR behind an acceleration structure; null
/// when the handle finds none.
[[nodiscard]] VkAccelerationStructureKHR native_acceleration(Device& device, AccelerationStructure structure);

/// The VkImage behind a texture; null when the handle finds none.
[[nodiscard]] VkImage native_image(Device& device, Texture texture);

/// The VkImageView for a view, made the first time it is asked for and kept
/// with the texture; null when the handle finds no texture.
[[nodiscard]] VkImageView native_view(Device& device, const TextureView& view);

/// The VkSampler behind a sampler; null when the handle finds none.
[[nodiscard]] VkSampler native_sampler(Device& device, Sampler sampler);

/// A handle to a buffer made outside the interface. The device never destroys
/// it: releasing the handle only forgets it, and the buffer's maker keeps it
/// alive at least as long as the handle.
[[nodiscard]] Owned<Buffer> adopt(Device& device, VkBuffer buffer, uint64_t size);

/// A handle to an image made outside the interface, described by `info`, with
/// `whole_view` (which may be null) as the view of all of it. The device
/// never destroys the image or that view: releasing the handle forgets it and
/// destroys only the views asked of it since. The image's maker keeps it alive
/// at least as long as the handle.
[[nodiscard]] Owned<Texture> adopt(Device& device, VkImage image, VkImageView whole_view,
                                   const TextureInfo& info);

/// `adopt` for an image described by the create info it was made from: the
/// usual way to hand the interface an image made before it, right after
/// making it. A format `Format` does not name leaves the info's undefined;
/// the texture then has only `whole_view`.
[[nodiscard]] Owned<Texture> adopt(Device& device, VkImage image, VkImageView whole_view,
                                   const VkImageCreateInfo& made_as);

/// The queue families a buffer that `Upload` writes is shared across, for
/// such a buffer made outside the interface.
[[nodiscard]] std::span<const uint32_t> upload_families(Device& device);

/// The aspect a barrier on a texture names; colour when the handle finds
/// none.
[[nodiscard]] VkImageAspectFlags native_aspect(Device& device, Texture texture);

/// Makes pipelines through the engine's pipeline cache, which the engine loads
/// and saves; null stops using one. The cache must outlive its use here.
void use_pipeline_cache(Device& device, VkPipelineCache cache);

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

/// The VkDescriptorSetLayout behind a shared layout, for pipelines still made
/// natively that bind its groups; null when the handle finds none.
[[nodiscard]] VkDescriptorSetLayout native_set_layout(const Device& device, SharedLayout layout);

/// Keeps `object` alive until the GPU has finished the frame recording now,
/// then lets its destructor run: for what was made before the interface and
/// has no handle to release.
template <typename T>
void retire(Device& device, T object);

/// Runs `fn` once the GPU has finished the frame recording now: for native
/// handles whose destruction needs more than a destructor.
void defer(Device& device, std::move_only_function<void()> fn);

/// Destroys everything released, whatever frame it waits for. The GPU must
/// be idle.
void release_all(Device& device);

/// Waits until the GPU has finished everything submitted: for code outside
/// the interface that rewrites descriptor sets every frame slot reads.
void wait_idle(Device& device);

template <typename T>
void retire(Device& device, T object) {
    defer(device, [doomed = std::move(object)] { (void)doomed; });
}

/// Makes `CommandList::transition` describe each barrier it records as a
/// line appended to `lines`, resources by the names their transitions carry
/// (the frame graph's barrier trace); null stops it.
void trace_transitions(Device& device, std::string* lines);

/// The command buffer a list records into, for code that still records
/// Vulkan itself.
[[nodiscard]] VkCommandBuffer native_command_buffer(CommandList& list);

/// The device's one-frame descriptor sets, for passes that acquire their own.
[[nodiscard]] FrameDescriptorCache& frame_cache(Device& device);
[[nodiscard]] const FrameDescriptorCache& frame_cache(const Device& device);

/// Reads back the times of the zones that finished, once a frame into a
/// command buffer on the graphics queue. Nothing in builds without the
/// profiler.
void collect_zones(Device& device, VkCommandBuffer cb);

/// A command list recording into a command buffer that code still hands
/// around: what it records lands in order with what is recorded into `cb`
/// directly. Each starts with nothing bound.
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
