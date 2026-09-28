#pragma once

#include "gpu/clear.hpp"
#include "gpu/format.hpp"
#include "gpu/usage.hpp"

#include <vulkan/vulkan.h>

namespace fjell::gpu::vulkan {

/// The Vulkan format for `format`; `VK_FORMAT_UNDEFINED` for `Format::undefined`.
[[nodiscard]] VkFormat to_vk(Format format);

/// The interface's name for a Vulkan format, for what the driver chooses (the
/// swapchain's format) and for images made before the interface. Returns
/// `Format::undefined` for a format the interface does not have.
[[nodiscard]] Format from_vk(VkFormat format);

/// The Vulkan format of a vertex attribute.
[[nodiscard]] VkFormat to_vk(VertexFormat format);

/// The Vulkan sample count, which is the same number.
[[nodiscard]] VkSampleCountFlagBits to_vk(Samples samples);

/// The Vulkan usage for `uses`, with copies and clears always allowed: as on
/// Metal, which asks no usage of a blit, any texture may be the source or the
/// destination of one.
[[nodiscard]] VkImageUsageFlags to_vk(TextureUses uses);

/// The Vulkan usage for `uses`, with copies and fills always allowed.
[[nodiscard]] VkBufferUsageFlags to_vk(BufferUses uses);

/// `clear` as Vulkan reads it for a texture of `format`: depth and stencil for
/// a depth format, unsigned integers for `color_uint`, floats otherwise.
[[nodiscard]] VkClearValue to_vk(const Clear& clear, Format format);

} // namespace fjell::gpu::vulkan
