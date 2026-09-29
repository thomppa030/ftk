#pragma once

#include "renderer/frustum.hpp"
#include "renderer/gpu/buffer.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/resources/mega_buffer.hpp"

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
struct GpuDrawData;

/// Per-RT-instance data, accessible in closest-hit shader via gl_InstanceID.
struct GpuRtInstance {
    uint32_t material_id;
    uint32_t first_index;    // into global index buffer
    uint32_t vertex_offset;  // into global vertex buffer
    uint32_t flags;          // RT_INSTANCE_* bits
};

/// GpuRtInstance::flags: the instance's material draws both sides of its
/// surfaces, so a ray reaching either side has hit the surface's front.
/// RT_INSTANCE_TWO_SIDED in fjell_gpu_types.glsl.
inline constexpr uint32_t RT_INSTANCE_TWO_SIDED = 1U;

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
class Device;
}

/// Manages BLAS + TLAS for DDGI ray tracing.
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

    /// Info about a mesh particle emitter to extract into the TLAS.
    struct ParticleEmitterInfo {
        uint32_t base_offset;     // into mega particle buffer
        uint32_t max_particles;
        uint32_t material_id;
        float avg_particle_size;
        const Blas* blas;
    };

    /// One host-side TLAS instance. Everything whose count is known on the CPU
    /// — mesh draws, terrain, and whatever comes next — reduces to this, so
    /// adding a source means building these rather than extending build_tlas.
    ///
    /// Particles are deliberately not expressible here: their instances are
    /// written by a compute shader into a region reserved from their upper
    /// bound, because the count is not known until the GPU has culled them.
    struct RtInstance {
        glm::mat4 transform{1.0F};
        uint32_t material_id{0};
        const Blas* blas{nullptr};
        /// The material draws both sides (its shader culls no faces, or
        /// the front ones), so neither side is a back face to a ray.
        bool two_sided{false};
    };

    /// How far into the world a pass's rays travel, so the TLAS is built to
    /// cover it. Every pass that traces against the TLAS declares this each
    /// frame, before the build.
    ///
    /// There is one TLAS and several passes tracing it, each to its own
    /// distance. Built to one pass's reach, it silently truncates the others:
    /// their rays leave the covered region, hit nothing, and take the miss
    /// shader — which reads as sky reflected off surfaces that should mirror
    /// the scene. The union is the only size that serves every tracer, and
    /// declaring it is what keeps a pass that changes its trace distance from
    /// quietly breaking a pass that did not.
    void register_trace_reach(const AABB& reach);

    /// The union of this frame's declared reaches. Invalid (`!valid()`) when
    /// nothing declared one, which means no pass is tracing and the caller
    /// should skip the build rather than cover the world.
    [[nodiscard]] AABB required_reach() const { return trace_reach_; }

    /// Forget the declared reaches, at the start of a frame's gathering.
    void clear_trace_reach() { trace_reach_ = {}; }

    /// Rebuild the TLAS from host instances plus GPU-generated particles.
    void build_tlas(VkCommandBuffer cmd, uint32_t frame_index,
                    const std::vector<RtInstance>& host_instances,
                    const std::vector<ParticleEmitterInfo>& particle_emitters = {});

    /// Create compute pipeline for particle → TLAS extraction (call once).
    void create_particle_tlas_pipeline(VkPipelineCache pipeline_cache);

    /// Cache the particle system's mega buffer handle for compute dispatch.
    void set_particle_ssbo(VkBuffer buffer) { particle_ssbo_ = buffer; }

    [[nodiscard]] VkAccelerationStructureKHR tlas(uint32_t frame_index) const { return frames_[frame_index].tlas_handle; }
    [[nodiscard]] bool tlas_valid(uint32_t frame_index) const { return frames_[frame_index].tlas_handle != VK_NULL_HANDLE; }
    [[nodiscard]] VkBuffer instance_data_buffer(uint32_t frame_index) const {
        auto& f = frames_[frame_index];
        return f.rt_instance_data ? f.rt_instance_data->handle() : VK_NULL_HANDLE;
    }
    [[nodiscard]] uint32_t instance_data_count(uint32_t frame_index) const { return frames_[frame_index].rt_instance_count; }

private:
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

    // The union of what this frame's tracers asked to be able to reach.
    AABB trace_reach_{};

    std::unordered_map<const Model*, std::vector<Blas>> blas_map_;
    std::unordered_map<const void*, Blas> entry_blas_map_;

    // Particle → TLAS compute pipeline
    VkDescriptorSetLayout particle_tlas_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout particle_tlas_pipe_layout_{VK_NULL_HANDLE};
    VkPipeline particle_tlas_pipeline_{VK_NULL_HANDLE};
    VkDescriptorPool particle_tlas_pool_{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> particle_tlas_sets_{};
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> particle_instance_buffers_;
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> particle_counter_buffers_;
    uint32_t particle_instance_capacity_{0};
    VkBuffer particle_ssbo_{VK_NULL_HANDLE};  // cached from particle system

    // Per-frame TLAS (triple-buffered to avoid in-flight destruction)
    struct FrameTlas {
        VkAccelerationStructureKHR tlas_handle{VK_NULL_HANDLE};
        std::unique_ptr<Buffer> tlas_buffer;
        std::unique_ptr<Buffer> tlas_scratch;
        std::unique_ptr<Buffer> instance_buffer;
        std::unique_ptr<Buffer> rt_instance_data;
        uint32_t rt_instance_count{0};
    };
    std::array<FrameTlas, MAX_FRAMES_IN_FLIGHT> frames_{};
};

} // namespace fjell
