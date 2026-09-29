#pragma once

#include "renderer/frustum.hpp"
#include "renderer/gpu/buffer.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/resources/mega_buffer.hpp"
#include "renderer/rt_instance.hpp"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include "renderer/renderer_constants.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace fjell {

class Model;
class ParticleSystem;
class PipelineCache;
class Scene;
struct DrawCommand;
struct GpuDrawData;

/// One bottom-level acceleration structure (BLAS) over one mesh entry, so
/// gl_PrimitiveID in a hit shader indexes that entry's triangles from
/// first_index and a TLAS instance carries exactly one material.
struct Blas {
    VkAccelerationStructureKHR handle{VK_NULL_HANDLE};
    // Shared by every BLAS built in the same batch: a model's submeshes live
    // in one buffer at 256-byte offsets rather than one allocation each.
    std::shared_ptr<Buffer> buffer;
    VkDeviceAddress device_address{0};
    uint32_t first_index{0};    // the entry's first_index in MegaBuffer
    uint32_t vertex_offset{0};  // the entry's vertex_offset in MegaBuffer
    // MegaBuffer::generation() when this BLAS baked its absolute vertex/index
    // device addresses. If the mega-buffer later grows (new device address),
    // this goes stale and the BLAS must be rebuilt before the TLAS references
    // it — otherwise traceRay dereferences freed memory (GPU READ_INVALID).
    uint32_t mega_generation{0};
};

class UploadContext;
namespace gpu {
class CommandList;
class Device;
}

/// The ray tracing acceleration structures every tracer shares: a BLAS per
/// mesh, and each frame one TLAS per scene over what that scene's rays can
/// reach.
///
/// A frame goes: begin_frame(), then prepare_scene() for the scene the frame
/// renders (on the CPU, before anything is recorded), then record_build()
/// from the graph, once per scene however many viewports render it, before
/// any pass traces. Tracers then ask for the scene's TLAS, which exists only
/// if it was built this frame.
class AccelStructureManager {
public:
    AccelStructureManager(Device& device, VmaAllocator allocator,
                          VkCommandPool command_pool, VkQueue queue,
                          UploadContext& upload, gpu::Device& gpu);
    ~AccelStructureManager();

    AccelStructureManager(const AccelStructureManager&) = delete;
    AccelStructureManager& operator=(const AccelStructureManager&) = delete;
    AccelStructureManager(AccelStructureManager&&) = delete;
    AccelStructureManager& operator=(AccelStructureManager&&) = delete;

    /// Info about a mesh particle emitter to extract into the TLAS.
    struct ParticleEmitterInfo {
        uint32_t base_offset;     // into mega particle buffer
        uint32_t max_particles;
        uint32_t material_id;
        float avg_particle_size;
        // The particle mesh's BLAS, by value: a BLAS the mega-buffer's
        // growth forces a rebuild of is retired, not freed, so its address
        // stays good for the frame that was prepared against it.
        VkDeviceAddress blas_address;
        uint32_t first_index;
        uint32_t vertex_offset;
        /// RT_MASK_* of the emitter's particles, by how they are drawn.
        uint32_t mask;
    };

    /// Starts a frame. A TLAS answers for the frame it was built in and no
    /// other, and a scene not prepared for a few frames gives its structures
    /// back.
    void begin_frame(uint64_t frame_serial);

    /// Makes ready, on the CPU, everything `scene`'s rays can hit this frame:
    /// the BLAS of each surface in `surfaces` (its surfaces within every
    /// tracer's reach, from SceneGatherer::gather_rt()), its terrain's ray
    /// mesh, its mesh particles, and the TLAS instances over them. Blocks on
    /// BLAS builds, so it runs before the frame is recorded.
    void prepare_scene(const Scene& scene, std::span<const DrawCommand> surfaces,
                       const PipelineCache* pipelines, const ParticleSystem* particles,
                       MegaBuffer& mega);

    /// Records the build of what prepare_scene() made ready for `scene_id`,
    /// once per frame: the first viewport to get here builds it and the rest
    /// trace what it built. Returns how many instances it built, 0 when it
    /// built nothing.
    uint32_t record_build(gpu::CommandList& commands, uint64_t scene_id, uint32_t frame_index);

    /// This frame's TLAS for `scene_id`, or VK_NULL_HANDLE when there is none:
    /// no tracer asked, nothing was in reach, or it is not built yet.
    [[nodiscard]] VkAccelerationStructureKHR tlas(uint64_t scene_id, uint32_t frame_index) const;
    /// The GpuRtInstance records the hit code reads beside that TLAS, one per
    /// instance in instance order.
    [[nodiscard]] VkBuffer instance_data_buffer(uint64_t scene_id, uint32_t frame_index) const;

    /// Create compute pipeline for particle → TLAS extraction (call once).
    void create_particle_tlas_pipeline(VkPipelineCache pipeline_cache);

private:
    /// Build or retrieve the BLASes for a Model: one per LOD 0 mesh entry.
    void ensure_blas(const Model* model, const MegaBuffer& mega);

    /// Build or retrieve BLAS from a raw MegaMeshEntry (terrain, particle
    /// mesh primitives). Uses the entry pointer as the map key.
    ///
    /// `geometry_changed` forces a rebuild for geometry whose vertices were
    /// overwritten in place — sculpted terrain. Nothing has moved, so the
    /// structure still points at valid memory and the mega-buffer generation
    /// has not changed, but it describes triangles that are no longer where
    /// they were.
    void ensure_blas_entry(const void* key, const MegaMeshEntry& entry, const MegaBuffer& mega,
                           bool geometry_changed = false);

    /// Get BLAS for a raw entry key (nullptr if not built).
    [[nodiscard]] const Blas* find_blas(const void* key) const;

    /// A model's BLASes, parallel to its LOD 0 mega entries: element i covers
    /// entry lod_levels()[0].first_mega_entry + i, and an entry without
    /// triangles holds a null handle. Empty until ensure_blas has run. Models
    /// keep their own map, keyed by Model* and rebuilt when the mega-buffer
    /// moves, separate from find_blas's mesh-entry map.
    [[nodiscard]] std::span<const Blas> model_blases(const Model* model) const;

    VkDeviceAddress buffer_address(VkBuffer buffer) const;

    // Build one BLAS per entry in a single buffer with a single submit. The
    // result is parallel to `entries`; an entry without triangles yields a
    // Blas with a null handle. Blocks until the GPU has finished building.
    [[nodiscard]] std::vector<Blas> build_blases(std::span<const MegaMeshEntry> entries,
                                                 const MegaBuffer& mega);

    // Destroys a stale BLAS once the frames that may trace it are done.
    void retire_blas(Blas blas);

    Device& device_;
    VkDevice vk_device_;
    VmaAllocator allocator_;
    VkCommandPool command_pool_;
    VkQueue queue_;
    UploadContext& upload_;
    /// Keeps what a rebuild replaces alive until the frames using it are
    /// done: an RT pass recorded earlier in the same frame may already have
    /// bound it, so freeing it inline is a dangling read (DEVICE_LOST).
    gpu::Device& gpu_;
    VkDeviceSize scratch_alignment_{256}; // minAccelerationStructureScratchOffsetAlignment

    std::unordered_map<const Model*, std::vector<Blas>> blas_map_;
    std::unordered_map<const void*, Blas> entry_blas_map_;

    // Particle → TLAS compute pipeline, shared by every scene
    VkDescriptorSetLayout particle_tlas_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout particle_tlas_pipe_layout_{VK_NULL_HANDLE};
    VkPipeline particle_tlas_pipeline_{VK_NULL_HANDLE};
    VkBuffer particle_ssbo_{VK_NULL_HANDLE};  // the particle system's state

    // One frame slot's TLAS for one scene. Each slot has its own, so a frame
    // still tracing never sees the next frame's build.
    struct FrameTlas {
        VkAccelerationStructureKHR tlas_handle{VK_NULL_HANDLE};
        std::unique_ptr<Buffer> tlas_buffer;
        std::unique_ptr<Buffer> tlas_scratch;
        std::unique_ptr<Buffer> instance_buffer;
        std::unique_ptr<Buffer> rt_instance_data;
        // Where the particle compute appends its instances, and the set that
        // binds this slot's buffers to it.
        std::unique_ptr<Buffer> particle_counter;
        VkDescriptorSet particle_set{VK_NULL_HANDLE};
        // The frame this slot was last built in.
        uint64_t built_serial{0};
    };

    // What one scene's rays can hit, as prepare_scene() left it for the
    // build.
    struct SceneTlas {
        std::vector<RtInstance> host_instances;
        std::vector<ParticleEmitterInfo> particle_emitters;
        uint64_t prepared_serial{0};
        // The scene's own sets for the particle compute, made the first
        // time it has mesh particles.
        VkDescriptorPool particle_pool{VK_NULL_HANDLE};
        std::array<FrameTlas, MAX_FRAMES_IN_FLIGHT> frames{};
    };

    // Builds `scene`'s TLAS into `frame` from its prepared instances.
    // Returns the instances built, 0 when there was nothing to build.
    uint32_t build_tlas(VkCommandBuffer cmd, SceneTlas& scene, FrameTlas& frame);
    // Gives a scene's structures back once the frames that may trace them
    // are done.
    void retire_scene(SceneTlas scene);

    std::unordered_map<uint64_t, SceneTlas> scenes_;
    uint64_t frame_serial_{0};
};

} // namespace fjell
