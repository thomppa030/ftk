#pragma once

#include "ftk/gpu/clear.hpp"
#include "ftk/gpu/compare.hpp"
#include "ftk/gpu/format.hpp"
#include "ftk/gpu/pipeline.hpp"
#include "ftk/gpu/render_encoder.hpp"
#include "ftk/gpu/sampler.hpp"
#include "ftk/gpu/shader.hpp"
#include "ftk/gpu/texture.hpp"
#include "ftk/gpu/usage.hpp"

#include <vulkan/vulkan.h>

#include <optional>

namespace ftk::gpu::vulkan {

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

/// The interface's sample count for a Vulkan one, for targets made before the
/// interface. A count above eight reads as eight, the most the interface has.
[[nodiscard]] Samples from_vk(VkSampleCountFlagBits samples);

/// The Vulkan usage for `uses`, with copies and clears always allowed: as on
/// Metal, which asks no usage of a blit, any texture may be the source or the
/// destination of one.
[[nodiscard]] VkImageUsageFlags to_vk(TextureUses uses);

/// The Vulkan usage for a texture of `format` with `uses`: `to_vk(uses)`, and
/// a sampled depth texture is a depth attachment too, which the read-only
/// depth layout it is sampled in asks of an image.
[[nodiscard]] VkImageUsageFlags to_vk(TextureUses uses, Format format);

/// The Vulkan usage for `uses`, with copies and fills always allowed.
[[nodiscard]] VkBufferUsageFlags to_vk(BufferUses uses);

/// `clear` as Vulkan reads it for a texture of `format`: depth and stencil for
/// a depth format, unsigned integers for `color_uint`, floats otherwise.
[[nodiscard]] VkClearValue to_vk(const Clear& clear, Format format);

/// The Vulkan filter for magnification and minification.
[[nodiscard]] VkFilter to_vk(Filter filter);

/// The Vulkan filter between mip levels.
[[nodiscard]] VkSamplerMipmapMode to_vk_mipmap(Filter filter);

[[nodiscard]] VkSamplerAddressMode to_vk(Address address);
[[nodiscard]] VkBorderColor to_vk(Border border);
[[nodiscard]] VkCompareOp to_vk(Compare compare);

/// The Vulkan view type of a resolved view (`ViewKind::automatic` never
/// reaches here: `resolve()` has chosen a kind).
[[nodiscard]] VkImageViewType to_vk(ViewKind kind);

/// Every aspect a texture of `format` has, for whole-image operations
/// (barriers, clears): depth and stencil together for a depth-stencil format.
[[nodiscard]] VkImageAspectFlags image_aspects(Format format);

/// The aspect a view of a Vulkan format shows a shader, for an image made
/// before the interface in a format `Format` may not name.
[[nodiscard]] VkImageAspectFlags view_aspect(VkFormat format);

/// The aspect a view of `format` shows a shader: depth alone for a
/// depth-stencil format, since a sampled view may show only one.
[[nodiscard]] VkImageAspectFlags view_aspect(Format format);

/// The Vulkan stages for `stages`.
[[nodiscard]] VkShaderStageFlags to_vk(ShaderStages stages);

/// The Vulkan descriptor type of a binding.
[[nodiscard]] VkDescriptorType to_vk(BindingKind kind);

[[nodiscard]] VkBlendFactor to_vk(BlendFactor factor);
[[nodiscard]] VkPrimitiveTopology to_vk(Topology topology);
[[nodiscard]] VkCullModeFlags to_vk(Cull cull);
[[nodiscard]] VkFrontFace to_vk(FrontFace front_face);
[[nodiscard]] VkPolygonMode to_vk(Fill fill);
[[nodiscard]] VkColorComponentFlags to_vk(ChannelMask channels);

/// An attachment's load and store operations.
[[nodiscard]] VkAttachmentLoadOp to_vk(Load load);
[[nodiscard]] VkAttachmentStoreOp to_vk(Store store);

/// How a depth attachment resolves.
[[nodiscard]] VkResolveModeFlagBits to_vk(DepthResolve resolve);

[[nodiscard]] VkIndexType to_vk(IndexType type);

/// The layout a sampled view of `format` is read in, the one the frame graph
/// leaves it in: read-only depth for a depth format, shader-read otherwise.
[[nodiscard]] VkImageLayout sampled_layout(Format format);

} // namespace ftk::gpu::vulkan
