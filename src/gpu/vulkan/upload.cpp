#include "gpu/upload.hpp"

#include "gpu/vulkan/access.hpp"
#include "gpu/vulkan/device_impl.hpp"
#include "gpu/vulkan/native.hpp"
#include "gpu/vulkan/translate.hpp"
#include "renderer/gpu/upload_context.hpp"

#include <algorithm>
#include <string>

// Uploads on the Vulkan device: bytes staged in the upload context's ring and
// copied on its lanes, buffers on the transfer queue where there is one,
// textures on the graphics queue, where their mips are filtered. A texture's
// barriers are the command list's, from the accesses the caller names.

namespace fjell::gpu {

namespace {

void report(Device& device, const std::string& why) {
    device.impl().report_once("Upload: " + why);
}

} // namespace

void Upload::to_buffer(Buffer dst, uint64_t offset, std::span<const std::byte> bytes) {
    const auto* record = device_->impl().buffers.get(dst);
    if (record == nullptr) {
        report(*device_, "the buffer no longer exists");
        return;
    }
    if (offset > record->size || bytes.size() > record->size - offset) {
        report(*device_, "a write of " + std::to_string(bytes.size()) + " bytes at " +
                             std::to_string(offset) + " reaches past the buffer's end (" +
                             std::to_string(record->size) + " bytes)");
        return;
    }
    if (bytes.empty()) return;
    impl_->lanes.upload_buffer(record->buffer, offset, bytes.data(), bytes.size());
}

void Upload::to_texture(const TextureView& dst, const TextureRegion& region,
                        std::span<const std::byte> bytes, const TextureUploadDesc& desc) {
    const auto* record = device_->impl().textures.get(dst.texture);
    if (record == nullptr) {
        report(*device_, "the texture no longer exists");
        return;
    }
    const ResolvedView view = resolve(dst, record->info);
    if (view.mip_count != 1) {
        report(*device_, "a texture is written one mip at a time");
        return;
    }
    const uint32_t texel = texel_size(view.format);
    if (texel == 0) {
        report(*device_, "the texture's format is not written from the CPU");
        return;
    }
    const uint32_t mip_width = std::max(1U, record->info.width >> view.base_mip);
    const uint32_t mip_height = std::max(1U, record->info.height >> view.base_mip);
    const uint32_t mip_depth = std::max(1U, record->info.depth >> view.base_mip);
    const TextureRegion box = region.width == 0
        ? TextureRegion{.width = mip_width, .height = mip_height, .depth = mip_depth}
        : region;
    if (box.height == 0 || box.depth == 0 || box.x + box.width > mip_width ||
        box.y + box.height > mip_height || box.z + box.depth > mip_depth) {
        report(*device_, "the region does not fit in the mip");
        return;
    }
    const bool whole_mip = box.width == mip_width && box.height == mip_height &&
                           box.depth == mip_depth;
    if (desc.generate_mips && (view.base_mip != 0 || !whole_mip)) {
        report(*device_, "mips are filled from the whole of mip 0");
        return;
    }
    const uint64_t needed = uint64_t{texel} * box.width * box.height * box.depth * view.layer_count;
    if (bytes.size() != needed) {
        report(*device_, "the region takes " + std::to_string(needed) + " bytes, " +
                             std::to_string(bytes.size()) + " were given");
        return;
    }

    const StagingSlice staging = impl_->lanes.stage_for_image(bytes.data(), bytes.size());
    vulkan::CommandBufferList recorder(*device_, impl_->lanes.image_cb());
    CommandList& cmd = recorder.list();
    // Filling the mips writes every one of them.
    const TextureView written = desc.generate_mips ? TextureView(dst.texture) : dst;
    cmd.barrier(written, desc.before, Access::copy_dst);

    VkBufferImageCopy copy{};
    copy.bufferOffset = staging.offset;
    copy.imageSubresource = {vulkan::view_aspect(view.format), view.base_mip, view.base_layer,
                             view.layer_count};
    copy.imageOffset = {static_cast<int32_t>(box.x), static_cast<int32_t>(box.y),
                        static_cast<int32_t>(box.z)};
    copy.imageExtent = {box.width, box.height, box.depth};
    vkCmdCopyBufferToImage(vulkan::native_command_buffer(cmd), staging.buffer, record->image,
                           vulkan::image_scope(Access::copy_dst, false).layout, 1, &copy);

    if (desc.generate_mips) cmd.generate_mipmaps(dst.texture);
    cmd.barrier(written, Access::copy_dst, desc.after);
}

void Upload::clear(const TextureView& view, const Clear& value, AccessSet after) {
    if (device_->impl().textures.get(view.texture) == nullptr) {
        report(*device_, "the texture no longer exists");
        return;
    }
    vulkan::CommandBufferList recorder(*device_, impl_->lanes.image_cb());
    CommandList& cmd = recorder.list();
    cmd.barrier(view, {}, Access::clear);
    cmd.clear(view, value);
    cmd.barrier(view, Access::clear, after);
}

} // namespace fjell::gpu
