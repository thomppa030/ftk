#pragma once

#include "renderer/renderer_constants.hpp"
#include "renderer/resources/mega_buffer.hpp"
#include "renderer/rt_instance.hpp"

#include <gpu/acceleration.hpp>
#include <gpu/buffer.hpp>
#include <gpu/owned.hpp>
#include <gpu/pipeline.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace fjell {

class Model;
class ParticleSystem;
class PassBuilder;
class PipelineCache;
class Scene;
struct DeclareContext;
struct DrawCommand;

namespace gpu {
class CommandList;
class Device;
}

/// One bottom-level acceleration structure (BLAS) over one mesh entry, so a
/// hit's primitive index counts that entry's triangles from first_index and
/// a TLAS instance carries exactly one material.
struct Blas {
    gpu::Owned<gpu::AccelerationStructure> structure;
    uint32_t first_index{0};    // the entry's first_index in MegaBuffer
    uint32_t vertex_offset{0};  // the entry's vertex_offset in MegaBuffer
};

/// The ray tracing acceleration structures every tracer shares: a BLAS per
/// mesh, and each frame one TLAS per scene over what that scene's rays can
/// reach.
///
/// A frame goes: begin_frame(), then prepare_scene() for the scene the frame
/// renders (on the CPU, before anything is declared or recorded), which
/// makes the structures the frame needs; then record_build() from the
/// `rt_scene` pass, once per scene however many viewports render it, which
/// builds the BLASes made since and the scene's TLAS. The pass declares the
/// TLAS and the instance table as its writes (`TLAS_NAME`, `INSTANCES_NAME`)
/// and the tracers as their reads, so the graph orders every trace after the
/// build.
class AccelStructureManager {
public:
    /// What `rt_scene` exports the scene's TLAS and its instance table
    /// under, for the tracers to declare their reads by.
    static constexpr const char* TLAS_NAME = "rt_scene_tlas";
    static constexpr const char* INSTANCES_NAME = "rt_scene_instances";

    explicit AccelStructureManager(gpu::Device& device);

    /// Declares a tracer's reads of the scene's TLAS and the hit data beside
    /// it, which order its traces after `rt_scene`'s build: what every pass
    /// that traces declares.
    static void declare_trace(PassBuilder& builder, const DeclareContext& ctx);

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
        // What instance records name the particle mesh's BLAS by.
        uint64_t blas_reference;
        uint32_t first_index;
        uint32_t vertex_offset;
        /// RT_MASK_* of the emitter's particles, by how they are drawn.
        uint32_t mask;
    };

    /// Starts a frame. A TLAS answers for the frame it was built in and no
    /// other, and a scene not prepared for a few frames gives its structures
    /// back.
    void begin_frame(uint64_t frame_serial);

    /// Makes ready, on the CPU, everything `scene`'s rays can hit this frame
    /// in frame slot `frame_index`: a BLAS for each surface in `surfaces`
    /// (its surfaces within every tracer's reach, from
    /// SceneGatherer::gather_rt()), its terrain's ray mesh and its mesh
    /// particles, and a TLAS large enough for the instances over them. New
    /// BLASes wait for record_build(); nothing here waits for the GPU.
    void prepare_scene(const Scene& scene, std::span<const DrawCommand> surfaces,
                       const PipelineCache* pipelines, const ParticleSystem* particles,
                       MegaBuffer& mega, uint32_t frame_index);

    /// Records the builds of the BLASes made since the last, then of what
    /// prepare_scene() made ready for `scene_id`, once per frame: the first
    /// viewport to get here builds it and the rest trace what it built. A
    /// frame slot whose TLAS was built from exactly this is not built again.
    /// Returns how many instances the TLAS holds, 0 when there is none or
    /// this frame's was already recorded.
    uint32_t record_build(gpu::CommandList& commands, uint64_t scene_id, uint32_t frame_index);

    /// The TLAS prepare_scene() made ready this frame for `scene_id`, and
    /// the instance table beside it: what `rt_scene` declares it writes.
    /// Invalid when the scene was not prepared this frame or nothing is in
    /// reach.
    [[nodiscard]] gpu::AccelerationStructure prepared_tlas(uint64_t scene_id, uint32_t frame_index) const;
    [[nodiscard]] gpu::Buffer prepared_instances(uint64_t scene_id, uint32_t frame_index) const;

    /// This frame's TLAS for `scene_id`, invalid when there is none: no
    /// tracer asked, nothing was in reach, or it is not built yet.
    [[nodiscard]] gpu::AccelerationStructure tlas(uint64_t scene_id, uint32_t frame_index) const;
    /// The GpuRtInstance records the hit code reads beside that TLAS, one per
    /// instance in instance order.
    [[nodiscard]] gpu::Buffer instance_data_buffer(uint64_t scene_id, uint32_t frame_index) const;

private:
    /// Makes the BLASes for a Model, one per LOD 0 mesh entry, to be built
    /// by the next record_build().
    void ensure_blas(const Model* model, const MegaBuffer& mega);

    /// Makes a BLAS for a raw MegaMeshEntry (terrain, particle mesh
    /// primitives), keyed by `key`.
    ///
    /// `geometry_changed` makes it anew for geometry whose vertices were
    /// overwritten in place — sculpted terrain: the structure describes
    /// triangles that are no longer where they were, and a frame still
    /// tracing it must keep the old one.
    void ensure_blas_entry(const void* key, const MegaMeshEntry& entry, const MegaBuffer& mega,
                           bool geometry_changed = false);

    /// Get BLAS for a raw entry key (nullptr if not made).
    [[nodiscard]] const Blas* find_blas(const void* key) const;

    /// A model's BLASes, parallel to its LOD 0 mega entries: element i covers
    /// entry lod_levels()[0].first_mega_entry + i, and an entry without
    /// triangles holds an empty one. Empty until ensure_blas has run.
    [[nodiscard]] std::span<const Blas> model_blases(const Model* model) const;

    /// Makes one BLAS per entry, sharing one allocation, each waiting for
    /// the next record_build(). The result is parallel to `entries`; an entry
    /// without triangles yields an empty one.
    [[nodiscard]] std::vector<Blas> make_blases(std::span<const MegaMeshEntry> entries,
                                                const MegaBuffer& mega);

    gpu::Device& device_;

    std::unordered_map<const Model*, std::vector<Blas>> blas_map_;
    std::unordered_map<const void*, Blas> entry_blas_map_;

    /// BLASes made and not yet built, with the triangles each is built
    /// from.
    struct PendingBuild {
        gpu::AccelerationStructure blas;
        gpu::Triangles triangles;
    };
    std::vector<PendingBuild> pending_builds_;

    /// Writes the mesh particles' instances, shared by every scene; invalid
    /// when it could not be made, and particles are left out.
    gpu::Owned<gpu::ComputePipeline> particle_pipeline_;
    gpu::Buffer particle_state_{};  // the particle system's state

    // One frame slot's TLAS for one scene. Each slot has its own, so a frame
    // still tracing never sees the next frame's build.
    struct FrameTlas {
        gpu::Owned<gpu::AccelerationStructure> tlas;
        /// The instances `tlas` was made for, and what the buffers hold
        /// room for.
        uint32_t capacity{0};
        /// The instance records the TLAS is built from, written by the CPU
        /// and, for particles, by the GPU.
        gpu::Owned<gpu::Buffer> instances;
        /// The GpuRtInstance records the hit code reads, in instance order.
        gpu::Owned<gpu::Buffer> rt_instance_data;
        /// Where the particle compute appends its instances.
        gpu::Owned<gpu::Buffer> particle_counter;
        // What the TLAS was built from. It still holds while the scene lists
        // the same instances; never after a build with particles, whose
        // instances the GPU writes anew each frame.
        std::vector<RtInstance> built_from;
        bool reusable{false};
        uint32_t instance_count{0};
        // The last frame this slot's TLAS was built for or found to hold
        // in; it answers for that frame only.
        uint64_t current_serial{0};
    };

    // What one scene's rays can hit, as prepare_scene() left it for the
    // build.
    struct SceneTlas {
        /// The instances listed on the CPU, their transforms checked.
        std::vector<RtInstance> host_instances;
        std::vector<ParticleEmitterInfo> particle_emitters;
        /// Instances the TLAS holds this frame at most: the listed ones and
        /// room for every particle.
        uint32_t instance_count{0};
        uint64_t prepared_serial{0};
        std::array<FrameTlas, MAX_FRAMES_IN_FLIGHT> frames{};
    };

    /// Makes `frame`'s TLAS and buffers large enough for `count` instances,
    /// keeping them when they already are. False when they could not be
    /// made, which is reported.
    [[nodiscard]] bool reserve(FrameTlas& frame, uint32_t count);

    // Records the build of `scene`'s TLAS into `frame` from its prepared
    // instances. Returns the instances built.
    uint32_t build_tlas(gpu::CommandList& commands, SceneTlas& scene, FrameTlas& frame);

    std::unordered_map<uint64_t, SceneTlas> scenes_;
    uint64_t frame_serial_{0};
};

} // namespace fjell
