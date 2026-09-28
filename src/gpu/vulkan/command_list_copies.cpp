#include "gpu/vulkan/command_list_impl.hpp"

#include "gpu/vulkan/access.hpp"
#include "gpu/vulkan/translate.hpp"
#include "renderer/gpu/gpu_core.hpp"

#include <algorithm>
#include <string>
#include <vector>

// A command list's copies, clears, mip generation and in-pass barriers. Each
// command's checks are made first, into what the command records; a command
// that cannot be done is reported once and not recorded.

namespace fjell::gpu {

namespace {

using TextureRecord = Device::Impl::TextureRecord;
using BufferRecord = Device::Impl::BufferRecord;

// A texture found for a command, with the view resolved against it.
struct Found {
    const TextureRecord* record{nullptr};
    ResolvedView view;
};

// A buffer range found for a command, `REST` counted out.
struct FoundRange {
    VkBuffer buffer{VK_NULL_HANDLE};
    uint64_t offset{0};
    uint64_t size{0};
};

Result<Found> find(Device::Impl& device, const TextureView& view) {
    const TextureRecord* record = device.textures.get(view.texture);
    if (record == nullptr) return make_error("the texture no longer exists");
    return Found{record, resolve(view, record->info)};
}

Result<FoundRange> find(Device::Impl& device, const BufferRange& range) {
    const BufferRecord* record = device.buffers.get(range.buffer);
    if (record == nullptr) return make_error("the buffer no longer exists");
    if (range.offset > record->size) return make_error("the range starts past the buffer's end");
    const uint64_t size =
        range.size == BufferRange::REST ? record->size - range.offset : range.size;
    if (range.offset + size > record->size) {
        return make_error("the range reaches past the buffer's end");
    }
    return FoundRange{record->buffer, range.offset, size};
}

VkExtent3D mip_extent(const TextureInfo& info, uint32_t level) {
    return {std::max(1U, info.width >> level), std::max(1U, info.height >> level),
            std::max(1U, info.depth >> level)};
}

VkOffset3D far_corner(const VkExtent3D& extent) {
    return {static_cast<int32_t>(extent.width), static_cast<int32_t>(extent.height),
            static_cast<int32_t>(extent.depth)};
}

VkImageSubresourceLayers layers_of(const Found& found, uint32_t mip) {
    return {vulkan::view_aspect(found.view.format), mip, found.view.base_layer,
            found.view.layer_count};
}

bool is_depth(Format format) {
    const FormatKind k = kind(format);
    return k == FormatKind::depth || k == FormatKind::depth_stencil;
}

void report(Device& device, const std::string& what, const std::string& why) {
    device.impl().report_once(what + ": " + why);
}

struct BufferCopy {
    VkBuffer from{VK_NULL_HANDLE};
    VkBuffer to{VK_NULL_HANDLE};
    VkBufferCopy region{};
};

Result<BufferCopy> buffer_copy(Device::Impl& device, const BufferRange& src,
                               const BufferRange& dst) {
    auto from = find(device, src);
    if (!from) return std::unexpected(from.error());
    auto to = find(device, dst);
    if (!to) return std::unexpected(to.error());
    if (from->size > to->size) {
        return make_error(std::to_string(from->size) + " bytes into a range of " +
                          std::to_string(to->size));
    }
    return BufferCopy{from->buffer, to->buffer, {from->offset, to->offset, from->size}};
}

struct TextureCopy {
    VkImage from{VK_NULL_HANDLE};
    VkImage to{VK_NULL_HANDLE};
    std::vector<VkImageCopy> regions;
};

Result<TextureCopy> texture_copy(Device::Impl& device, const TextureView& src,
                                 const TextureView& dst) {
    auto from = find(device, src);
    if (!from) return std::unexpected(from.error());
    auto to = find(device, dst);
    if (!to) return std::unexpected(to.error());
    if (from->view.mip_count != to->view.mip_count ||
        from->view.layer_count != to->view.layer_count) {
        return make_error("the views have different mips or layers");
    }
    const uint32_t texel = texel_size(from->view.format);
    if (texel == 0 || texel != texel_size(to->view.format)) {
        return make_error("the formats' texels differ in size");
    }

    TextureCopy copy{from->record->image, to->record->image, {}};
    for (uint32_t i = 0; i < from->view.mip_count; ++i) {
        const VkExtent3D extent = mip_extent(from->record->info, from->view.base_mip + i);
        const VkExtent3D other = mip_extent(to->record->info, to->view.base_mip + i);
        if (extent.width != other.width || extent.height != other.height ||
            extent.depth != other.depth) {
            return make_error("the views differ in size");
        }
        VkImageCopy region{};
        region.srcSubresource = layers_of(*from, from->view.base_mip + i);
        region.dstSubresource = layers_of(*to, to->view.base_mip + i);
        region.extent = extent;
        copy.regions.push_back(region);
    }
    return copy;
}

struct BufferTextureCopy {
    VkBuffer buffer{VK_NULL_HANDLE};
    VkImage image{VK_NULL_HANDLE};
    VkBufferImageCopy region{};
};

// A buffer and one mip of a texture as a copy between them: the texture's
// texels packed row after row, layer after layer.
Result<BufferTextureCopy> buffer_texture_copy(Device::Impl& device, const BufferRange& range,
                                              const TextureView& view) {
    auto texture = find(device, view);
    if (!texture) return std::unexpected(texture.error());
    if (texture->view.mip_count != 1) return make_error("a buffer is copied to or from one mip");
    const uint32_t texel = texel_size(texture->view.format);
    if (texel == 0) return make_error("the texture's format is not copied to or from buffers");
    auto buffer = find(device, range);
    if (!buffer) return std::unexpected(buffer.error());

    const VkExtent3D extent = mip_extent(texture->record->info, texture->view.base_mip);
    const uint64_t needed = uint64_t{texel} * extent.width * extent.height * extent.depth *
                            texture->view.layer_count;
    if (buffer->size < needed) {
        return make_error("the mip takes " + std::to_string(needed) + " bytes, the range holds " +
                          std::to_string(buffer->size));
    }
    BufferTextureCopy copy{buffer->buffer, texture->record->image, {}};
    copy.region.bufferOffset = buffer->offset;
    copy.region.imageSubresource = layers_of(*texture, texture->view.base_mip);
    copy.region.imageExtent = extent;
    return copy;
}

Result<FoundRange> fill_range(Device::Impl& device, const BufferRange& range) {
    auto found = find(device, range);
    if (!found) return found;
    if (found->offset % 4 != 0 || found->size % 4 != 0) {
        return make_error("the range's offset and size are not multiples of four");
    }
    return found;
}

// A texture whose mips can be filtered down one from another on this queue.
Result<const TextureRecord*> mip_chain(Device::Impl& device, Texture texture, Queue queue) {
    if (queue == Queue::compute) return make_error("mips are filtered down on the graphics queue");
    const TextureRecord* record = device.textures.get(texture);
    if (record == nullptr) return make_error("the texture no longer exists");
    if (kind(record->info.format) != FormatKind::color) {
        return make_error("only a colour texture read as floats is filtered down");
    }
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(device.core.physical_device(),
                                        vulkan::to_vk(record->info.format), &properties);
    constexpr VkFormatFeatureFlags NEEDED = VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                                            VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                            VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if ((properties.optimalTilingFeatures & NEEDED) != NEEDED) {
        return make_error("this GPU cannot filter the texture's format");
    }
    return record;
}

Result<VkImageMemoryBarrier2> image_barrier(Device::Impl& device, const TextureView& view,
                                            AccessSet before, AccessSet after, Queue queue) {
    auto found = find(device, view);
    if (!found) return std::unexpected(found.error());
    if (after.empty()) return make_error("a barrier to no access orders nothing");
    const bool depth = is_depth(found->view.format);
    const auto from = vulkan::image_scope(before, depth);
    const auto to = vulkan::image_scope(after, depth);
    if (!from || !to) return make_error("the accesses on one side need different layouts");

    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = from->stages;
    barrier.srcAccessMask = from->access;
    barrier.dstStageMask = to->stages;
    barrier.dstAccessMask = to->access;
    if (queue == Queue::compute) {
        barrier.srcStageMask = vulkan::compute_queue_stages(barrier.srcStageMask);
        barrier.dstStageMask = vulkan::compute_queue_stages(barrier.dstStageMask);
    }
    barrier.oldLayout = from->layout;
    barrier.newLayout = to->layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = found->record->image;
    barrier.subresourceRange = {vulkan::image_aspects(found->view.format), found->view.base_mip,
                                found->view.mip_count, found->view.base_layer,
                                found->view.layer_count};
    return barrier;
}

Result<VkBufferMemoryBarrier2> buffer_barrier(Device::Impl& device, const BufferRange& range,
                                              AccessSet before, AccessSet after, Queue queue) {
    auto found = find(device, range);
    if (!found) return std::unexpected(found.error());
    if (before.empty() || after.empty()) {
        return make_error("a buffer barrier orders one access behind another");
    }
    const vulkan::BufferScope from = vulkan::buffer_scope(before);
    const vulkan::BufferScope to = vulkan::buffer_scope(after);

    VkBufferMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    barrier.srcStageMask = from.stages;
    barrier.srcAccessMask = from.access;
    barrier.dstStageMask = to.stages;
    barrier.dstAccessMask = to.access;
    if (queue == Queue::compute) {
        barrier.srcStageMask = vulkan::compute_queue_stages(barrier.srcStageMask);
        barrier.dstStageMask = vulkan::compute_queue_stages(barrier.dstStageMask);
    }
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = found->buffer;
    barrier.offset = found->offset;
    barrier.size = found->size;
    return barrier;
}

} // namespace

void CommandList::copy(BufferRange src, BufferRange dst) {
    if (!vulkan::outside_render(*device_, *impl_, "copies")) return;
    const auto copy = buffer_copy(device_->impl(), src, dst);
    if (!copy) {
        report(*device_, "Buffer copy", copy.error());
        return;
    }
    vkCmdCopyBuffer(impl_->cb, copy->from, copy->to, 1, &copy->region);
}

void CommandList::copy(const TextureView& src, const TextureView& dst) {
    if (!vulkan::outside_render(*device_, *impl_, "copies")) return;
    const auto copy = texture_copy(device_->impl(), src, dst);
    if (!copy) {
        report(*device_, "Texture copy", copy.error());
        return;
    }
    vkCmdCopyImage(impl_->cb, copy->from, vulkan::image_scope(Access::copy_src, false).layout,
                   copy->to, vulkan::image_scope(Access::copy_dst, false).layout,
                   static_cast<uint32_t>(copy->regions.size()), copy->regions.data());
}

void CommandList::copy(BufferRange src, const TextureView& dst) {
    if (!vulkan::outside_render(*device_, *impl_, "copies")) return;
    const auto copy = buffer_texture_copy(device_->impl(), src, dst);
    if (!copy) {
        report(*device_, "Buffer to texture copy", copy.error());
        return;
    }
    vkCmdCopyBufferToImage(impl_->cb, copy->buffer, copy->image,
                           vulkan::image_scope(Access::copy_dst, false).layout, 1, &copy->region);
}

void CommandList::copy(const TextureView& src, BufferRange dst) {
    if (!vulkan::outside_render(*device_, *impl_, "copies")) return;
    const auto copy = buffer_texture_copy(device_->impl(), dst, src);
    if (!copy) {
        report(*device_, "Texture to buffer copy", copy.error());
        return;
    }
    vkCmdCopyImageToBuffer(impl_->cb, copy->image,
                           vulkan::image_scope(Access::copy_src, false).layout, copy->buffer, 1,
                           &copy->region);
}

void CommandList::clear(const TextureView& view, const Clear& value) {
    if (!vulkan::outside_render(*device_, *impl_, "clears")) return;
    const auto found = find(device_->impl(), view);
    if (!found) {
        report(*device_, "Clear", found.error());
        return;
    }
    const Format format = found->view.format;
    const VkImageSubresourceRange range{vulkan::image_aspects(format), found->view.base_mip,
                                        found->view.mip_count, found->view.base_layer,
                                        found->view.layer_count};
    const VkImageLayout layout = vulkan::image_scope(Access::clear, is_depth(format)).layout;
    const VkClearValue clear_value = vulkan::to_vk(value, format);
    if (is_depth(format)) {
        vkCmdClearDepthStencilImage(impl_->cb, found->record->image, layout,
                                    &clear_value.depthStencil, 1, &range);
    } else {
        vkCmdClearColorImage(impl_->cb, found->record->image, layout, &clear_value.color, 1,
                             &range);
    }
}

void CommandList::fill(BufferRange range, uint32_t value) {
    if (!vulkan::outside_render(*device_, *impl_, "fills")) return;
    const auto found = fill_range(device_->impl(), range);
    if (!found) {
        report(*device_, "Fill", found.error());
        return;
    }
    vkCmdFillBuffer(impl_->cb, found->buffer, found->offset, found->size, value);
}

void CommandList::generate_mipmaps(Texture texture) {
    if (!vulkan::outside_render(*device_, *impl_, "generates mips")) return;
    const auto chain = mip_chain(device_->impl(), texture, impl_->queue);
    if (!chain) {
        report(*device_, "Mipmaps", chain.error());
        return;
    }
    const TextureRecord& record = **chain;
    const TextureInfo& info = record.info;

    // Each mip goes from written to read before the next is filtered from
    // it; all but the last go back to written together at the end.
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = record.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, info.layers};
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;

    for (uint32_t level = 1; level < info.mips; ++level) {
        barrier.subresourceRange.baseMipLevel = level - 1;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier2(impl_->cb, &dependency);

        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, info.layers};
        blit.srcOffsets[1] = far_corner(mip_extent(info, level - 1));
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, info.layers};
        blit.dstOffsets[1] = far_corner(mip_extent(info, level));
        vkCmdBlitImage(impl_->cb, record.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, record.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    }

    if (info.mips < 2) return;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = info.mips - 1;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_NONE;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    vkCmdPipelineBarrier2(impl_->cb, &dependency);
}

void CommandList::barrier(const TextureView& view, AccessSet before, AccessSet after) {
    if (!vulkan::outside_render(*device_, *impl_, "places a barrier")) return;
    const auto barrier = image_barrier(device_->impl(), view, before, after, impl_->queue);
    if (!barrier) {
        report(*device_, "Barrier", barrier.error());
        return;
    }
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &*barrier;
    vkCmdPipelineBarrier2(impl_->cb, &dependency);
}

void CommandList::barrier(BufferRange range, AccessSet before, AccessSet after) {
    if (!vulkan::outside_render(*device_, *impl_, "places a barrier")) return;
    const auto barrier = buffer_barrier(device_->impl(), range, before, after, impl_->queue);
    if (!barrier) {
        report(*device_, "Barrier", barrier.error());
        return;
    }
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers = &*barrier;
    vkCmdPipelineBarrier2(impl_->cb, &dependency);
}

} // namespace fjell::gpu
