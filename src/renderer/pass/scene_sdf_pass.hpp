#pragma once

#include "renderer/pass/render_pass.hpp"
#include "renderer/frustum.hpp"
#include "renderer/gpu/buffer.hpp"

#include <glm/glm.hpp>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace fjell {

class Device;
class Model;
class TerrainAsset;
struct PassResources;

// What an instance's grid holds: a baked mesh distance field, or a terrain's
// height samples that the shader turns into a distance on the fly.
enum class SdfInstanceKind : uint32_t { mesh = 0, terrain = 1 };

// Per-instance SDF metadata for the compositing shader (matches GLSL SdfInstance)
struct GpuSdfInstance {
    glm::mat4 world_to_sdf;    // inverse model matrix scaled to SDF grid space
    glm::vec3 sdf_min;         // local-space SDF volume min
    float sdf_scale;           // max_dist for denormalizing int16 values
    glm::vec3 sdf_max;         // local-space SDF volume max
    uint32_t grid_offset;      // offset into mesh SDF buffer (in int16 units)
    glm::uvec3 grid_resolution;
    uint32_t pad0;
    glm::vec3 mesh_min;        // local-space mesh bounds, tighter than the grid
    float pad1;
    glm::vec3 mesh_max;
    float pad2;
    // Terrain instances only: the field's placement, so the shader can go
    // from a world position to a height sample without a matrix.
    SdfInstanceKind kind{SdfInstanceKind::mesh};
    uint32_t pad3;
    glm::vec2 terrain_origin;      // world XZ of the field's min corner
    float terrain_size;            // metres across
    float terrain_height_scale;    // world metres per unit of normalized height
    float terrain_height_offset;   // world Y of normalized height 0
    float pad4;
};
static_assert(sizeof(GpuSdfInstance) == 176);

class SceneSdfPass : public RenderPass {
public:
    static constexpr uint32_t DEFAULT_RESOLUTION = 128;
    static constexpr VkFormat SDF_FORMAT = VK_FORMAT_R16_SFLOAT;

    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext&) override {}
    void post_create(GpuContext& gpu, PassRegistry& passes) override;
    void register_resources(ResourceRegistry& registry) override;

    // Legacy create — forwards to standard create
    void create(Device& device, VkDescriptorSetLayout global_set_layout, VmaAllocator allocator,
                VkPipelineCache pipeline_cache = VK_NULL_HANDLE);

    // Register a model's per-mesh SDF data. Returns true if data was uploaded.
    bool register_model_sdf(const Model& model, uintptr_t model_id);

    /// Where a terrain's height samples live in the packed grid buffer.
    struct TerrainSdfEntry {
        uint32_t offset;       // in int16 units
        uint32_t samples;      // per side
        uint32_t generation;   // TerrainAsset::edit_generation() the copy matches
    };

    /// Upload a terrain's heights into the grid buffer, once. A terrain has no
    /// baked distance field; the shader derives the distance from the heights
    /// it finds here. Null when the buffer is full.
    const TerrainSdfEntry* register_terrain_sdf(const TerrainAsset& terrain);

    /// Re-copy a rectangle of a registered terrain's heights after a sculpt
    /// edit, in place, and record the asset generation the copy now matches.
    void update_terrain_sdf(const TerrainAsset& terrain, uint32_t x, uint32_t z,
                            uint32_t size_x, uint32_t size_z);

    /// Mark a world-space box for recomputation without any instance having
    /// moved: what an edit inside a terrain needs.
    void invalidate_region(const AABB& world_bounds) { add_dirty_region(world_bounds); }

    /// Identifies one placed mesh across frames. Node ids repeat across
    /// scenes, so the scene id is part of the key.
    [[nodiscard]] static uint64_t instance_key(uint64_t scene_id, uint32_t object_id) {
        return (scene_id << 32) | object_id;
    }

    /// The field is rebuilt only where it can have changed. Each frame the
    /// caller hands over every placed mesh between begin_gather() and
    /// end_gather(); the pass remembers where each one was and turns moved,
    /// added and vanished instances into voxel regions for compose_pending().
    void begin_gather();
    /// `world_bounds` is the mesh's bounding box in world space, `moved` says
    /// its transform changed since the previous frame.
    void gather_instance(uint64_t key, const AABB& world_bounds, bool moved,
                         const GpuSdfInstance& instance);
    void end_gather();

    [[nodiscard]] bool has_pending_update() const {
        return full_rebuild_ || !dirty_regions_.empty();
    }
    /// Records the outstanding region updates, or a full rebuild, into `cmd`.
    void compose_pending(VkCommandBuffer cmd);

    // Set world-space bounds of the global SDF volume
    void set_bounds(const glm::vec3& min, const glm::vec3& max);

    /// Distances are clamped here: a voxel farther than this from every
    /// surface holds exactly this value. Sphere tracing stays conservative,
    /// and a moved mesh only touches voxels within this band of its bounds.
    [[nodiscard]] float max_distance() const;

    [[nodiscard]] VkImageView view() const { return sdf_view_; }
    [[nodiscard]] VkSampler sampler() const { return sdf_sampler_; }
    [[nodiscard]] VkImage image() const { return sdf_image_; }
    [[nodiscard]] const glm::vec3& volume_min() const { return volume_min_; }
    [[nodiscard]] const glm::vec3& volume_max() const { return volume_max_; }
    [[nodiscard]] uint32_t resolution() const { return resolution_; }

    // Debug slice visualization
    void render_slice(VkCommandBuffer cmd, uint32_t axis, float slice_pos);
    [[nodiscard]] VkDescriptorSet slice_imgui_set() const { return slice_imgui_set_; }

    // Per-model SDF metadata
    struct MeshSdfEntry {
        uint32_t offset;      // in int16 units
        uint32_t resolution;
        glm::vec3 sdf_min;
        glm::vec3 sdf_max;
        glm::vec3 mesh_min;
        glm::vec3 mesh_max;
        float max_dist;
    };

    // Lookup a registered model's SDF entry (returns nullptr if not found)
    [[nodiscard]] const MeshSdfEntry* model_entry(uintptr_t model_id) const;

private:
    void create_volume_image();
    void create_compute_pipeline();

    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineCache vk_pipeline_cache_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};

    // Global SDF 3D texture
    VkImage sdf_image_{VK_NULL_HANDLE};
    VmaAllocation sdf_memory_{VK_NULL_HANDLE};
    VkImageView sdf_view_{VK_NULL_HANDLE};
    VkSampler sdf_sampler_{VK_NULL_HANDLE};

    // Compositing compute pipeline
    VkDescriptorSetLayout composite_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout composite_layout_{VK_NULL_HANDLE};
    VkPipeline composite_pipeline_{VK_NULL_HANDLE};
    VkDescriptorPool pool_{VK_NULL_HANDLE};
    VkDescriptorSet composite_set_{VK_NULL_HANDLE};

    // Per-mesh SDF data packed into a single SSBO
    std::unique_ptr<Buffer> mesh_sdf_buffer_;
    uint32_t mesh_sdf_offset_{0}; // next free offset in int16 units

    // Per-instance metadata SSBO
    std::unique_ptr<Buffer> instance_buffer_;

    // Volume config
    glm::vec3 volume_min_{-10.0f};
    glm::vec3 volume_max_{10.0f};
    uint32_t resolution_{DEFAULT_RESOLUTION};
    VkImageLayout image_layout_{VK_IMAGE_LAYOUT_UNDEFINED};

    std::unordered_map<uintptr_t, MeshSdfEntry> model_sdf_entries_;
    std::unordered_map<const TerrainAsset*, TerrainSdfEntry> terrain_sdf_entries_;

    // What the field currently holds, per placed mesh, so the next gather
    // can tell what moved or vanished.
    struct TrackedInstance {
        AABB bounds;
        uint32_t grid_offset;
        bool seen;
    };
    std::unordered_map<uint64_t, TrackedInstance> tracked_;

    // This frame's instances and their world bounds, parallel arrays
    std::vector<GpuSdfInstance> gathered_;
    std::vector<AABB> gathered_bounds_;

    // World-space boxes whose voxels must be recomputed, each already grown
    // by max_distance(). Merged where they overlap.
    std::vector<AABB> dirty_regions_;
    bool full_rebuild_{true};

    [[nodiscard]] glm::vec3 voxel_size() const;
    void add_dirty_region(AABB region);
    void compose_region(VkCommandBuffer cmd, const glm::ivec3& voxel_min,
                        const glm::ivec3& voxel_extent, uint32_t instance_offset,
                        uint32_t instance_count);

    // Slice visualization
    static constexpr uint32_t SLICE_SIZE = 256;
    VkImage slice_image_{VK_NULL_HANDLE};
    VmaAllocation slice_memory_{VK_NULL_HANDLE};
    VkImageView slice_view_{VK_NULL_HANDLE};
    VkSampler slice_sampler_{VK_NULL_HANDLE};
    VkPipelineLayout slice_layout_{VK_NULL_HANDLE};
    VkPipeline slice_pipeline_{VK_NULL_HANDLE};
    VkDescriptorSetLayout slice_set_layout_{VK_NULL_HANDLE};
    VkDescriptorPool slice_pool_{VK_NULL_HANDLE};
    VkDescriptorSet slice_set_{VK_NULL_HANDLE};
    VkDescriptorSet slice_imgui_set_{VK_NULL_HANDLE};
    bool slice_initialized_{false};
};

} // namespace fjell
