#include "renderer/resources/terrain_asset.hpp"

#include "core/log.hpp"
#include "renderer/gpu/image.hpp"
#include "renderer/gpu/upload_context.hpp"
#include "renderer/resources/material.hpp"
#include "renderer/resources/fjterrain_io.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace fjell {

namespace {

constexpr float UNORM16_MAX = 65535.0F;

constexpr VkPipelineStageFlags2 HEIGHT_READ_STAGE = VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT;
constexpr VkPipelineStageFlags2 WEIGHT_READ_STAGE = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;

// Weight texture of a terrain that has never been painted: layer 0 at full
// weight, everywhere.
constexpr std::array<uint8_t, 4> UNPAINTED_WEIGHT = {255, 0, 0, 0};

// The height texture is sampled by the terrain mesh shader, not by a fragment
// stage, so Image::transition_layout — which only knows fragment readers and
// only two layout pairs — cannot express its hazards. Every transition of a
// terrain image goes through here, against the stage that actually reads it:
// the mesh stage for heights, the fragment stage for layer weights.
void barrier_terrain_image(VkCommandBuffer cmd, VkImage image,
                           VkImageLayout old_layout, VkImageLayout new_layout,
                           VkPipelineStageFlags2 read_stage) {
    const VkPipelineStageFlags2 READ_STAGE = read_stage;

    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    if (old_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcStageMask = READ_STAGE;
        barrier.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    } else {
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        barrier.srcAccessMask = 0;
    }
    if (new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    } else {
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = READ_STAGE;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    }

    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace

TerrainAsset::~TerrainAsset() {
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, sampler_, nullptr);
    }
}

VkImageView TerrainAsset::height_view() const {
    return height_image_ ? height_image_->view() : VK_NULL_HANDLE;
}

VkImageView TerrainAsset::weight_view() const {
    return weight_image_ ? weight_image_->view() : VK_NULL_HANDLE;
}

const std::string& TerrainAsset::layer_material_path(uint32_t layer) const {
    static const std::string none;
    if (layer >= data_.layers.size()) return none;
    const uint32_t name = data_.layers[layer].material_path;
    return name < data_.names.size() ? data_.names[name] : none;
}

float TerrainAsset::layer_uv_scale(uint32_t layer) const {
    return layer < data_.layers.size() ? data_.layers[layer].uv_scale
                                       : FJTERRAIN_DEFAULT_LAYER_UV_SCALE;
}

Material* TerrainAsset::layer_material(uint32_t layer) const {
    return layer < layer_count() ? layer_materials_[layer] : nullptr;
}

uint32_t TerrainAsset::layer_material_id(uint32_t layer) const {
    const Material* material = layer_material(layer);
    if (material == nullptr) material = unassigned_material_;
    return material != nullptr ? material->material_id() : 0;
}

void TerrainAsset::set_layer_count(uint32_t count) {
    count = std::min(count, FJTERRAIN_MAX_LAYERS);
    if (count == layer_count()) return;
    // Every layer owns one name-table entry, at its own index, so the table
    // never needs deduplicating or garbage collecting.
    data_.layers.resize(count);
    data_.names.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        data_.layers[i].material_path = i;
        if (data_.layers[i].uv_scale <= 0.0F) {
            data_.layers[i].uv_scale = FJTERRAIN_DEFAULT_LAYER_UV_SCALE;
        }
    }
    for (uint32_t i = count; i < FJTERRAIN_MAX_LAYERS; ++i) {
        layer_materials_[i] = nullptr;
    }
    dirty_ = true;
}

void TerrainAsset::set_layer_material_path(uint32_t layer, const std::string& path) {
    if (layer >= layer_count()) return;
    const uint32_t name = data_.layers[layer].material_path;
    if (name >= data_.names.size()) return;
    if (data_.names[name] == path) return;
    data_.names[name] = path;
    dirty_ = true;
}

void TerrainAsset::set_layer_uv_scale(uint32_t layer, float metres_per_repeat) {
    if (layer >= layer_count()) return;
    const float clamped = std::max(metres_per_repeat, 0.01F);
    if (data_.layers[layer].uv_scale == clamped) return;
    data_.layers[layer].uv_scale = clamped;
    dirty_ = true;
}

void TerrainAsset::set_layer_material(uint32_t layer, Material* material) {
    if (layer < FJTERRAIN_MAX_LAYERS) layer_materials_[layer] = material;
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

    barrier_terrain_image(cmd, asset->height_image_->handle(), VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, HEIGHT_READ_STAGE);

    VkBufferImageCopy region{};
    region.bufferOffset = staging.offset;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {samples, samples, 1};
    vkCmdCopyBufferToImage(cmd, staging.buffer, asset->height_image_->handle(),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    barrier_terrain_image(cmd, asset->height_image_->handle(),
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, HEIGHT_READ_STAGE);

    asset->height_image_->create_view(device, VK_FORMAT_R16_UNORM,
                                      VK_IMAGE_ASPECT_COLOR_BIT, 1);

    // Layer weights. An unpainted field gets a texture at one texel per
    // height sample filled with layer 0, so the first paint stroke is a
    // partial upload like any other rather than a texture rebuild.
    {
        const auto& data = asset->data_;
        const bool painted = data.header.weight_count > 0 && !data.weights.empty();
        const uint32_t side = painted ? data.header.weight_count : samples;
        std::vector<uint8_t> unpainted;
        if (!painted) {
            unpainted.resize(static_cast<size_t>(side) * side * 4);
            for (size_t i = 0; i < unpainted.size(); i += 4) {
                std::copy(UNPAINTED_WEIGHT.begin(), UNPAINTED_WEIGHT.end(), &unpainted[i]);
            }
        }
        const uint8_t* weight_bytes = painted ? data.weights.data() : unpainted.data();
        const VkDeviceSize weight_size = static_cast<VkDeviceSize>(side) * side * 4;
        asset->weight_side_ = side;

        auto weight_image = Image::create(allocator, side, side, VK_FORMAT_R8G8B8A8_UNORM,
                                          VK_IMAGE_TILING_OPTIMAL,
                                          VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                              VK_IMAGE_USAGE_SAMPLED_BIT,
                                          VMA_MEMORY_USAGE_AUTO, 1);
        if (!weight_image) {
            return make_error("terrain: " + weight_image.error());
        }
        asset->weight_image_ = std::make_unique<Image>(std::move(*weight_image));

        StagingSlice weight_staging = upload.stage_for_image(weight_bytes, weight_size);
        barrier_terrain_image(cmd, asset->weight_image_->handle(), VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, WEIGHT_READ_STAGE);
        VkBufferImageCopy weight_region{};
        weight_region.bufferOffset = weight_staging.offset;
        weight_region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        weight_region.imageSubresource.layerCount = 1;
        weight_region.imageExtent = {side, side, 1};
        vkCmdCopyBufferToImage(cmd, weight_staging.buffer, asset->weight_image_->handle(),
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &weight_region);
        barrier_terrain_image(cmd, asset->weight_image_->handle(),
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, WEIGHT_READ_STAGE);
        asset->weight_image_->create_view(device, VK_FORMAT_R8G8B8A8_UNORM,
                                          VK_IMAGE_ASPECT_COLOR_BIT, 1);
    }

    // Linear filtering so heights interpolate between samples the same way the
    // CPU query does; clamp to edge so border patches read a defined height
    // instead of wrapping to the far side of the terrain. The weight texture
    // shares it: layer transitions should blend, not step.
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

    const glm::vec3 corner = local_min_corner();
    ray_vertices_.resize(static_cast<size_t>(count) * count);
    for (uint32_t z = 0; z < count; ++z) {
        for (uint32_t x = 0; x < count; ++x) {
            const uint32_t sx = std::min(x * step, samples - 1);
            const uint32_t sz = std::min(z * step, samples - 1);

            Vertex& v = ray_vertices_[(static_cast<size_t>(z) * count) + x];
            v.position = {corner.x + (static_cast<float>(sx) * spacing),
                          data_.world_height(height_at_sample(sx, sz)),
                          corner.z + (static_cast<float>(sz) * spacing)};
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

void TerrainAsset::ensure_height_range() const {
    if (!height_range_stale_) return;
    float lowest = 1.0F;
    float highest = 0.0F;
    for (float h : data_.heights) {
        lowest = std::min(lowest, h);
        highest = std::max(highest, h);
    }
    min_world_height_ = data_.world_height(lowest);
    max_world_height_ = data_.world_height(highest);
    height_range_stale_ = false;
}

float TerrainAsset::min_world_height() const {
    ensure_height_range();
    return min_world_height_;
}

float TerrainAsset::max_world_height() const {
    ensure_height_range();
    return max_world_height_;
}

void TerrainAsset::copy_heights(uint32_t x, uint32_t z, uint32_t size_x, uint32_t size_z,
                                float* out) const {
    const uint32_t samples = data_.sample_count();
    if (out == nullptr || x >= samples || z >= samples) return;
    const uint32_t x_end = std::min(x + size_x, samples);
    const uint32_t z_end = std::min(z + size_z, samples);
    for (uint32_t row = z; row < z_end; ++row) {
        const float* src = &data_.heights[(static_cast<size_t>(row) * samples) + x];
        float* dst = out + (static_cast<size_t>(row - z) * size_x);
        std::copy(src, src + (x_end - x), dst);
    }
}

void TerrainAsset::apply_heights(uint32_t x, uint32_t z, uint32_t size_x, uint32_t size_z,
                                 const float* heights, UploadContext& upload) {
    const uint32_t samples = data_.sample_count();
    if (heights == nullptr || !height_image_ || x >= samples || z >= samples) return;
    const uint32_t x_end = std::min(x + size_x, samples);
    const uint32_t z_end = std::min(z + size_z, samples);
    const uint32_t w = x_end - x;
    const uint32_t h = z_end - z;
    if (w == 0 || h == 0) return;

    // One walk both stores the clamped floats and quantizes the texels the
    // GPU copy wants, tightly packed at the rect's own width.
    std::vector<uint16_t> texels(static_cast<size_t>(w) * h);
    for (uint32_t row = 0; row < h; ++row) {
        float* dst = &data_.heights[(static_cast<size_t>(z + row) * samples) + x];
        const float* src = heights + (static_cast<size_t>(row) * size_x);
        uint16_t* tex = &texels[static_cast<size_t>(row) * w];
        for (uint32_t col = 0; col < w; ++col) {
            const float clamped = std::clamp(src[col], 0.0F, 1.0F);
            dst[col] = clamped;
            tex[col] = static_cast<uint16_t>(std::lround(clamped * UNORM16_MAX));
        }
    }

    StagingSlice staging = upload.stage_for_image(texels.data(), texels.size() * sizeof(uint16_t));
    VkCommandBuffer cmd = upload.image_cb();
    VkImage image = height_image_->handle();

    barrier_terrain_image(cmd, image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, HEIGHT_READ_STAGE);

    VkBufferImageCopy region{};
    region.bufferOffset = staging.offset;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {static_cast<int32_t>(x), static_cast<int32_t>(z), 0};
    region.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd, staging.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &region);

    barrier_terrain_image(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, HEIGHT_READ_STAGE);

    height_range_stale_ = true;
    dirty_ = true;
}

void TerrainAsset::copy_weights(uint32_t x, uint32_t z, uint32_t size_x, uint32_t size_z,
                                uint8_t* out) const {
    const uint32_t side = weight_side_;
    if (out == nullptr || x >= side || z >= side) return;
    const uint32_t x_end = std::min(x + size_x, side);
    const uint32_t z_end = std::min(z + size_z, side);
    for (uint32_t row = z; row < z_end; ++row) {
        uint8_t* dst = out + (static_cast<size_t>(row - z) * size_x * 4);
        if (data_.weights.empty()) {
            for (uint32_t col = 0; col < x_end - x; ++col) {
                std::copy(UNPAINTED_WEIGHT.begin(), UNPAINTED_WEIGHT.end(),
                          dst + (static_cast<size_t>(col) * 4));
            }
            continue;
        }
        const uint8_t* src = &data_.weights[((static_cast<size_t>(row) * side) + x) * 4];
        std::copy(src, src + (static_cast<size_t>(x_end - x) * 4), dst);
    }
}

void TerrainAsset::apply_weights(uint32_t x, uint32_t z, uint32_t size_x, uint32_t size_z,
                                 const uint8_t* rgba, UploadContext& upload) {
    const uint32_t side = weight_side_;
    if (rgba == nullptr || !weight_image_ || x >= side || z >= side) return;
    const uint32_t x_end = std::min(x + size_x, side);
    const uint32_t z_end = std::min(z + size_z, side);
    const uint32_t w = x_end - x;
    const uint32_t h = z_end - z;
    if (w == 0 || h == 0) return;

    if (data_.weights.empty()) {
        data_.weights.resize(static_cast<size_t>(side) * side * 4);
        for (size_t i = 0; i < data_.weights.size(); i += 4) {
            std::copy(UNPAINTED_WEIGHT.begin(), UNPAINTED_WEIGHT.end(), &data_.weights[i]);
        }
        data_.header.weight_count = side;
    }

    std::vector<uint8_t> texels(static_cast<size_t>(w) * h * 4);
    for (uint32_t row = 0; row < h; ++row) {
        uint8_t* dst = &data_.weights[((static_cast<size_t>(z + row) * side) + x) * 4];
        const uint8_t* src = rgba + (static_cast<size_t>(row) * size_x * 4);
        std::copy(src, src + (static_cast<size_t>(w) * 4), dst);
        std::copy(src, src + (static_cast<size_t>(w) * 4), &texels[static_cast<size_t>(row) * w * 4]);
    }

    StagingSlice staging = upload.stage_for_image(texels.data(), texels.size());
    VkCommandBuffer cmd = upload.image_cb();
    VkImage image = weight_image_->handle();

    barrier_terrain_image(cmd, image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, WEIGHT_READ_STAGE);

    VkBufferImageCopy region{};
    region.bufferOffset = staging.offset;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {static_cast<int32_t>(x), static_cast<int32_t>(z), 0};
    region.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd, staging.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &region);

    barrier_terrain_image(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, WEIGHT_READ_STAGE);

    dirty_ = true;
}

Result<> TerrainAsset::save() {
    auto result = save_fjterrain(path_, data_);
    if (result) dirty_ = false;
    return result;
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
