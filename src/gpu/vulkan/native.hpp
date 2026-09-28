#pragma once

#include "gpu/device.hpp"
#include "gpu/vulkan/frame_descriptor_cache.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <span>
#include <string>

// The bridge between the GPU interface and code that still speaks Vulkan,
// both ways, so either can use what the other made while files move onto the
// interface one at a time. It goes when the last file has moved.

namespace fjell::gpu::vulkan {

/// The VkBuffer behind a buffer; null when the handle finds none.
[[nodiscard]] VkBuffer native_buffer(Device& device, Buffer buffer);

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

/// Makes pipelines through the engine's pipeline cache, which the engine loads
/// and saves; null stops using one. The cache must outlive its use here.
void use_pipeline_cache(Device& device, VkPipelineCache cache);

/// The VkPipeline behind a pipeline; null when the handle finds none.
[[nodiscard]] VkPipeline native_pipeline(Device& device, ComputePipeline pipeline);
[[nodiscard]] VkPipeline native_pipeline(Device& device, GraphicsPipeline pipeline);

/// The VkPipelineLayout a pipeline binds with; null when the handle finds none.
[[nodiscard]] VkPipelineLayout native_layout(Device& device, ComputePipeline pipeline);
[[nodiscard]] VkPipelineLayout native_layout(Device& device, GraphicsPipeline pipeline);

/// Hands the device one of the engine's shared set layouts: its bindings,
/// read from the Vulkan description it was made from, and the set most
/// shaders declare it at. Pipelines that name the returned handle take it at
/// the set it fits. The layout stays the engine's, which keeps it while
/// pipelines naming it are made.
[[nodiscard]] SharedLayout share_layout(Device& device, std::string name, uint32_t usual_set,
                                        VkDescriptorSetLayout layout,
                                        std::span<const VkDescriptorSetLayoutBinding> bindings);

/// A handle to one of the engine's descriptor sets of a shared layout, for
/// binding where a pipeline names that layout. The device never writes or
/// frees it; the engine keeps it alive at least as long as the handle.
[[nodiscard]] Owned<BindGroup> adopt_group(Device& device, SharedLayout shared, VkDescriptorSet set);

/// The VkDescriptorSet behind a bind group; null when the handle finds none.
[[nodiscard]] VkDescriptorSet native_group(Device& device, BindGroup group);

/// Tells the device a frame slot starts recording, right after the slot's
/// fence wait: the sets and transient memory that lasted one frame in that
/// slot are free again.
void begin_frame(Device& device, uint32_t frame_slot);

/// The device's one-frame descriptor sets, for passes that acquire their own.
[[nodiscard]] FrameDescriptorCache& frame_cache(Device& device);
[[nodiscard]] const FrameDescriptorCache& frame_cache(const Device& device);

} // namespace fjell::gpu::vulkan
