#include "renderer/resources/terrain_asset.hpp"

#include "core/log.hpp"
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
    // The same walk that quantizes heights for the texture also measures the
    // range, which bounding-box fitting needs and the format does not store.
    std::vector<uint16_t> texels(static_cast<size_t>(samples) * samples);
    float lowest = 1.0F;
    float highest = 0.0F;
    for (size_t i = 0; i < texels.size(); ++i) {
        const float clamped = std::clamp(asset->data_.heights[i], 0.0F, 1.0F);
        lowest = std::min(lowest, clamped);
        highest = std::max(highest, clamped);
        texels[i] = static_cast<uint16_t>(std::lround(clamped * UNORM16_MAX));
    }
    asset->min_world_height_ = asset->data_.world_height(lowest);
    asset->max_world_height_ = asset->data_.world_height(highest);
    asset->build_ray_mesh();
    FJELL_GFX_DEBUG("Terrain '{}': {}x{} samples, ray mesh {} verts / {} tris ({:.1f} MB)",
                    path, samples, samples, asset->ray_vertices_.size(),
                    asset->ray_indices_.size() / 3,
                    static_cast<double>(asset->ray_vertices_.size() * sizeof(Vertex) +
                                        asset->ray_indices_.size() * sizeof(uint32_t)) / 1e6);
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

namespace {

// Every 4th sample: 256x256 vertices for a 1024-sample field, ~130k triangles
// at roughly 2 m spacing over 512 m. Rays do not resolve finer than that, and
// the full density would be 2M triangles for no visible gain.
constexpr uint32_t RAY_MESH_STEP = 4;

} // namespace

void TerrainAsset::build_ray_mesh() {
    const uint32_t samples = data_.sample_count();
    if (samples < 2) return;

    const uint32_t step = std::min(RAY_MESH_STEP, samples - 1);
    // Round up, so the last row lands on the final sample rather than short of
    // it. Truncating leaves a strip of missing ground along two edges that rays
    // pass straight through.
    const uint32_t count = ((samples - 1 + step - 1) / step) + 1;
    const float spacing = data_.header.world_size / static_cast<float>(samples - 1);

    ray_vertices_.resize(static_cast<size_t>(count) * count);
    for (uint32_t z = 0; z < count; ++z) {
        for (uint32_t x = 0; x < count; ++x) {
            const uint32_t sx = std::min(x * step, samples - 1);
            const uint32_t sz = std::min(z * step, samples - 1);

            Vertex& v = ray_vertices_[(static_cast<size_t>(z) * count) + x];
            v.position = {static_cast<float>(sx) * spacing,
                          data_.world_height(height_at_sample(sx, sz)),
                          static_cast<float>(sz) * spacing};
            v.color = glm::vec3{1.0F};
            v.tex_coord = {static_cast<float>(sx) / static_cast<float>(samples - 1),
                           static_cast<float>(sz) / static_cast<float>(samples - 1)};

            // Central differences over the decimated stride, matching the
            // geometry actually being built rather than the source grid.
            const uint32_t xm = sx >= step ? sx - step : sx;
            const uint32_t xp = std::min(sx + step, samples - 1);
            const uint32_t zm = sz >= step ? sz - step : sz;
            const uint32_t zp = std::min(sz + step, samples - 1);
            const float dx = static_cast<float>(xp - xm) * spacing;
            const float dz = static_cast<float>(zp - zm) * spacing;
            const float hl = data_.world_height(height_at_sample(xm, sz));
            const float hr = data_.world_height(height_at_sample(xp, sz));
            const float hd = data_.world_height(height_at_sample(sx, zm));
            const float hu = data_.world_height(height_at_sample(sx, zp));
            v.normal = glm::normalize(glm::vec3{dx > 0.0F ? -(hr - hl) / dx : 0.0F, 1.0F,
                                                dz > 0.0F ? -(hu - hd) / dz : 0.0F});
            v.tangent = glm::vec4{1.0F, 0.0F, 0.0F, 1.0F};
        }
    }

    ray_indices_.clear();
    ray_indices_.reserve(static_cast<size_t>(count - 1) * (count - 1) * 6);
    for (uint32_t z = 0; z + 1 < count; ++z) {
        for (uint32_t x = 0; x + 1 < count; ++x) {
            const uint32_t v0 = (z * count) + x;
            const uint32_t v1 = v0 + 1;
            const uint32_t v2 = v0 + count;
            const uint32_t v3 = v2 + 1;
            // Winding matches terrain.mesh so both agree on which side faces up.
            ray_indices_.insert(ray_indices_.end(), {v0, v2, v1, v1, v2, v3});
        }
    }
}

void TerrainAsset::upload_ray_mesh(MegaBuffer& mega) {
    if (ray_mesh_uploaded() || ray_indices_.empty()) return;
    ray_mesh_entry_ = mega.upload(ray_vertices_, ray_indices_);
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
