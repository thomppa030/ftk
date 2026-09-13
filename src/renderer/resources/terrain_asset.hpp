#pragma once

#include "core/result.hpp"
#include "renderer/resources/fjterrain.hpp"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <memory>
#include <string>

namespace fjell {

class Image;
class UploadContext;

/// A loaded .fjterrain: the CPU height data plus the GPU texture the mesh
/// shader samples to place vertices.
///
/// Heights deliberately live in two places and no more. The texture is
/// write-only from the CPU's side — geometry is generated from it every frame
/// and it is never read back — while the CPU array backs physics, queries and
/// editing. A third copy would be a third thing to keep in sync, and a height
/// array that disagrees with collision puts characters through visibly solid
/// ground.
class TerrainAsset {
public:
    TerrainAsset() = default;
    ~TerrainAsset();

    TerrainAsset(const TerrainAsset&) = delete;
    TerrainAsset& operator=(const TerrainAsset&) = delete;
    TerrainAsset(TerrainAsset&&) = delete;
    TerrainAsset& operator=(TerrainAsset&&) = delete;

    /// Load a .fjterrain and upload its heights as a single-channel texture.
    [[nodiscard]] static Result<std::unique_ptr<TerrainAsset>> load(
        const std::string& path, VkDevice device, VmaAllocator allocator,
        VkPhysicalDevice physical_device, UploadContext& upload);

    [[nodiscard]] const FjTerrainData& data() const { return data_; }
    [[nodiscard]] const std::string& path() const { return path_; }

    /// The height texture is not a `Texture`: that class hardcodes 4 bytes per
    /// pixel when sizing its staging copy, and heights are single-channel
    /// 16-bit. Image + sampler are owned here instead.
    [[nodiscard]] VkImageView height_view() const;
    [[nodiscard]] VkSampler height_sampler() const { return sampler_; }

    [[nodiscard]] uint32_t sample_count() const { return data_.sample_count(); }
    [[nodiscard]] uint32_t patch_quads() const { return data_.patch_quads(); }
    [[nodiscard]] uint32_t patches_per_side() const { return data_.patches_per_side(); }
    [[nodiscard]] float world_size() const { return data_.header.world_size; }
    [[nodiscard]] float height_scale() const { return data_.header.height_scale; }
    [[nodiscard]] float height_offset() const { return data_.header.height_offset; }

    /// World-space height range across the whole field, measured at load. The
    /// nominal range (offset..offset+scale) is what the format *allows*; this is
    /// what the data actually uses, which keeps a bounding box from claiming
    /// empty air above a flat terrain.
    [[nodiscard]] float min_world_height() const { return min_world_height_; }
    [[nodiscard]] float max_world_height() const { return max_world_height_; }

    /// Normalized height at a sample index, clamped at the edges.
    [[nodiscard]] float height_at_sample(uint32_t x, uint32_t z) const;

    /// World-space height at an arbitrary point in terrain-local XZ metres,
    /// bilinearly filtered to match what the shader reconstructs between
    /// samples. Outside the terrain this clamps to the edge.
    [[nodiscard]] float height_at(float local_x, float local_z) const;

private:
    FjTerrainData data_;
    std::string path_;
    float min_world_height_{0.0F};
    float max_world_height_{0.0F};
    VkDevice device_{VK_NULL_HANDLE};
    std::unique_ptr<Image> height_image_;
    VkSampler sampler_{VK_NULL_HANDLE};
};

} // namespace fjell
