#pragma once

#include "renderer/gpu/buffer.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/resources/mega_buffer.hpp"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include "renderer/renderer_constants.hpp"

#include <array>
#include <cstdint>
#include <memory>
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
    uint32_t pad;
};

/// One bottom-level acceleration structure (BLAS) per unique Model.
struct Blas {
    VkAccelerationStructureKHR handle{VK_NULL_HANDLE};
    std::unique_ptr<Buffer> buffer;
    VkDeviceAddress device_address{0};
    uint32_t first_index{0};    // LOD 0 first_index in MegaBuffer
    uint32_t vertex_offset{0};  // LOD 0 vertex_offset in MegaBuffer
    // MegaBuffer::generation() when this BLAS baked its absolute vertex/index
    // device addresses. If the mega-buffer later grows (new device address),
    // this goes stale and the BLAS must be rebuilt before the TLAS references
    // it — otherwise traceRay dereferences freed memory (GPU READ_INVALID).
    uint32_t mega_generation{0};
};

class UploadContext;

/// Manages BLAS + TLAS for DDGI ray tracing.
class AccelStructureManager {
public:
    AccelStructureManager(Device& device, VmaAllocator allocator,
                          VkCommandPool command_pool, VkQueue queue,
                          UploadContext& upload);
    ~AccelStructureManager();

    AccelStructureManager(const AccelStructureManager&) = delete;
    AccelStructureManager& operator=(const AccelStructureManager&) = delete;
    AccelStructureManager(AccelStructureManager&&) = delete;
    AccelStructureManager& operator=(AccelStructureManager&&) = delete;

    /// Build or retrieve BLAS for a Model. Call during model load.
    void ensure_blas(const Model* model, const MegaBuffer& mega);

    /// Build or retrieve BLAS from a raw MegaMeshEntry (for particle mesh primitives).
    /// Uses the entry pointer as the map key.
    void ensure_blas_entry(const void* key, const MegaMeshEntry& entry, const MegaBuffer& mega);

    /// Get BLAS for a raw entry key (nullptr if not built).
    [[nodiscard]] const Blas* find_blas(const void* key) const;

    /// Info about a mesh particle emitter to extract into the TLAS.
    struct ParticleEmitterInfo {
        uint32_t base_offset;     // into mega particle buffer
        uint32_t max_particles;
        uint32_t material_id;
        float avg_particle_size;
        const Blas* blas;
    };

    /// Rebuild TLAS from scene draws + particle emitters.
    void build_tlas(VkCommandBuffer cmd, uint32_t frame_index,
                    const GpuDrawData* draw_data, uint32_t draw_count,
                    const std::vector<const Model*>& draw_models,
                    const std::vector<ParticleEmitterInfo>& particle_emitters = {});

    /// Free per-frame TLAS/instance buffers + TLAS handles that build_tlas
    /// retired (on growth) the LAST time this slot was used. Call once per
    /// frame AFTER the slot's in-flight fence has signaled (begin_frame).
    /// build_tlas reallocates these per-frame buffers when the instance count
    /// grows; an RT-tracing pass recorded earlier in the SAME frame already
    /// bound the old buffer/TLAS, so freeing inline is a use-after-free that
    /// the GPU hits as a dangling read (DEVICE_LOST). Retire instead, reclaim
    /// here when the slot is provably idle.
    void reclaim_retired(uint32_t frame_index);

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
    void build_blas(const Model* model, const MegaBuffer& mega);

    // Destroy a BLAS's acceleration structure handle (GPU must be idle).
    // Used to retire a stale BLAS before rebuilding against a moved mega-buffer.
    void destroy_blas(Blas& blas);

    Device& device_;
    VkDevice vk_device_;
    VmaAllocator allocator_;
    VkCommandPool command_pool_;
    VkQueue queue_;
    UploadContext& upload_;
    VkDeviceSize scratch_alignment_{256}; // minAccelerationStructureScratchOffsetAlignment

    std::unordered_map<const Model*, Blas> blas_map_;
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

    // Per-slot graveyard: buffers/TLAS handles replaced by build_tlas growth,
    // kept alive until the slot's fence signals (reclaim_retired). See
    // reclaim_retired() for why inline free is a use-after-free here.
    std::array<std::vector<std::unique_ptr<Buffer>>, MAX_FRAMES_IN_FLIGHT> retired_buffers_;
    std::array<std::vector<VkAccelerationStructureKHR>, MAX_FRAMES_IN_FLIGHT> retired_tlas_;
};

} // namespace fjell
