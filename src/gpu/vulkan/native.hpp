#pragma once

#include "gpu/device.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>

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

} // namespace fjell::gpu::vulkan
