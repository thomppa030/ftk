#include "renderer/resources/terrain_asset.hpp"

#include "renderer/gpu/image.hpp"
#include "renderer/gpu/upload_context.hpp"
#include "renderer/resources/fjterrain_io.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace fjell {

namespace {

constexpr float UNORM16_MAX = 65535.0F;

} // namespace

TerrainAsset::~TerrainAsset() {
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, sampler_, nullptr);
    }
}

VkImageView TerrainAsset::height_view() const {
    return height_image_ ? height_image_->view() : VK_NULL_HANDLE;
}

Result<std::unique_ptr<TerrainAsset>> TerrainAsset::load(
    const std::string& path, VkDevice device, VmaAllocator allocator,
    VkPhysicalDevice /*physical_device*/, UploadContext& upload) {
    auto parsed = load_fjterrain(path);
    if (!parsed) {
        return make_error(parsed.error());
    }

    auto asset = std::make_unique<TerrainAsset>();
    asset->data_ = std::move(*parsed);
    asset->path_ = path;
    asset->device_ = device;

    const uint32_t samples = asset->data_.sample_count();

    // R16_UNORM regardless of the on-disk format: the shader wants a single
    // normalized channel, and 16 bits resolve finer than terrain authoring
    // does. No mips — sampling a coarser mip would move vertices, and the patch
    // grid already controls how densely the texture is read.
    std::vector<uint16_t> texels(static_cast<size_t>(samples) * samples);
    for (size_t i = 0; i < texels.size(); ++i) {
        const float clamped = std::clamp(asset->data_.heights[i], 0.0F, 1.0F);
        texels[i] = static_cast<uint16_t>(std::lround(clamped * UNORM16_MAX));
    }
    const VkDeviceSize bytes = texels.size() * sizeof(uint16_t);

    auto image = Image::create(allocator, samples, samples, VK_FORMAT_R16_UNORM,
                               VK_IMAGE_TILING_OPTIMAL,
                               VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                   VK_IMAGE_USAGE_SAMPLED_BIT,
                               VMA_MEMORY_USAGE_AUTO, 1);
    if (!image) {
        return make_error("terrain: " + image.error());
    }
    asset->height_image_ = std::make_unique<Image>(std::move(*image));

    StagingSlice staging = upload.stage_for_image(texels.data(), bytes);
    VkCommandBuffer cmd = upload.image_cb();

    asset->height_image_->transition_layout(cmd, VK_IMAGE_LAYOUT_UNDEFINED,
                                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1);

    VkBufferImageCopy region{};
    region.bufferOffset = staging.offset;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {samples, samples, 1};
    vkCmdCopyBufferToImage(cmd, staging.buffer, asset->height_image_->handle(),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    asset->height_image_->transition_layout(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1);

    asset->height_image_->create_view(device, VK_FORMAT_R16_UNORM,
                                      VK_IMAGE_ASPECT_COLOR_BIT, 1);

    // Linear filtering so heights interpolate between samples the same way the
    // CPU query does; clamp to edge so border patches read a defined height
    // instead of wrapping to the far side of the terrain.
    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.anisotropyEnable = VK_FALSE;
    sampler_info.maxAnisotropy = 1.0F;
    sampler_info.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    sampler_info.unnormalizedCoordinates = VK_FALSE;
    sampler_info.compareEnable = VK_FALSE;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.minLod = 0.0F;
    sampler_info.maxLod = 1.0F;
    sampler_info.mipLodBias = 0.0F;
    if (vkCreateSampler(device, &sampler_info, nullptr, &asset->sampler_) != VK_SUCCESS) {
        return make_error("terrain: failed to create height sampler");
    }

    return asset;
}

float TerrainAsset::height_at_sample(uint32_t x, uint32_t z) const {
    const uint32_t samples = data_.sample_count();
    if (samples == 0) return 0.0F;
    const uint32_t cx = std::min(x, samples - 1);
    const uint32_t cz = std::min(z, samples - 1);
    return data_.heights[(static_cast<size_t>(cz) * samples) + cx];
}

float TerrainAsset::height_at(float local_x, float local_z) const {
    const uint32_t samples = data_.sample_count();
    if (samples < 2) return data_.header.height_offset;

    const float quads = static_cast<float>(samples - 1);
    const float scale = quads / data_.header.world_size;
    const float fx = std::clamp(local_x * scale, 0.0F, quads);
    const float fz = std::clamp(local_z * scale, 0.0F, quads);

    const auto x0 = static_cast<uint32_t>(fx);
    const auto z0 = static_cast<uint32_t>(fz);
    const uint32_t x1 = std::min(x0 + 1, samples - 1);
    const uint32_t z1 = std::min(z0 + 1, samples - 1);
    const float tx = fx - static_cast<float>(x0);
    const float tz = fz - static_cast<float>(z0);

    const float h00 = height_at_sample(x0, z0);
    const float h10 = height_at_sample(x1, z0);
    const float h01 = height_at_sample(x0, z1);
    const float h11 = height_at_sample(x1, z1);

    const float top = h00 + ((h10 - h00) * tx);
    const float bottom = h01 + ((h11 - h01) * tx);
    return data_.world_height(top + ((bottom - top) * tz));
}

} // namespace fjell
