#pragma once

#include "renderer/systems/particle_budget.hpp"

#include "renderer/gpu/buffer.hpp"
#include "renderer/gpu/growable_buffer.hpp"
#include "renderer/components/particle_emitter.hpp"
#include "renderer/pass/render_pass.hpp"
#include "renderer/systems/render_subsystem.hpp"

#ifndef NDEBUG
#include "platform/file_watcher.hpp"
#endif
#include "renderer/resources/mega_buffer.hpp"
#include "renderer/systems/vfx_module.hpp"

#include <glm/glm.hpp>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include "renderer/renderer_constants.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_set>
#include <vector>

namespace fjell {

// One record per emitter slot, rewritten every dispatch. Matches the GLSL
// EmitterData in particle_common.glsl field for field. std430 rounds the
// array's stride up to the vec3s' 16-byte alignment, so the size has to be a
// multiple of 16 on both sides or the GPU reads every record after the first
// at the wrong offset; the trailing word keeps it there, and a change here
// is checked against the compiled shader's ArrayStride, not just sizeof.
struct GpuEmitterData {
    glm::vec3 emitter_position; float dt; // this emitter's step: zero while paused or frozen
    glm::vec3 emitter_velocity; uint32_t max_particles;
    uint32_t emit_count;
    uint32_t random_seed;
    uint32_t base_offset;
    uint32_t counter_offset;
    uint32_t module_offset;       // offset into module buffer (in uint32_t units)
    uint32_t flags;               // bit 0 trail, 1 lit, 2 mesh, 3 has sub-emitter, 4-5 its trigger
    float    trail_width;         // base width for trail strips
    float    trail_width_over_life; // 0=taper to zero, 1=constant width
    // Sub-emitter fields
    uint32_t sub_event_offset;    // offset into event buffer for this emitter
    uint32_t sub_event_source;    // parent emitter index (0xFFFFFFFF = not a child)
    uint32_t sub_burst_count;     // particles per event (child only)
    float    sub_vel_inherit;     // velocity inheritance factor (child only)
    // Mesh particle draw range (MegaBuffer), for GPU-side indirect command
    // construction — mesh entries never change per-emitter after resolve,
    // so this rides along with the rest of the per-emitter constants.
    uint32_t mesh_vertex_offset;
    uint32_t mesh_first_index;
    uint32_t mesh_index_count;
    uint32_t _pad;
};
static_assert(sizeof(GpuEmitterData) == 96);

// Push constants for lit particle fragment shader (12 bytes)
struct ParticleLitPush {
    float metallic;
    float roughness;
    uint32_t material_id; // 0 = no material, use push metallic/roughness
};
static_assert(sizeof(ParticleLitPush) == 12);

// Push constants for all compute shaders (16 bytes)
struct ParticleComputePush {
    float dt;
    uint32_t random_seed;
    uint32_t emitter_count;
    uint32_t total_max_particles;
    uint32_t sub_emit_pass; // 1 = only child emitters emit (post-sim)
};
static_assert(sizeof(ParticleComputePush) == 20);

class Device;
class Scene;

class ParticleSystem : public RenderSubsystem {
public:
    void create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void recreate_pipelines();

    // RenderSubsystem lifecycle
    void sync(const RenderSubsystemContext& ctx) override;
    /// The scene-wide particle and emitter budgets; see particle_budget.hpp.
    static constexpr uint32_t MAX_SCENE_PARTICLES = fjell::MAX_SCENE_PARTICLES;
    static constexpr uint32_t MAX_SCENE_EMITTERS = fjell::MAX_SCENE_EMITTERS;

    void update(const RenderSubsystemContext& ctx) override;
    void record_compute(const FrameContext& ctx) override;
    void collect_exports(DeclareContext& ctx) const override;
    void record_opaque(const FrameContext& ctx) override;
    void record_distortion(const FrameContext& ctx) override;
    void record_transparent(const FrameContext& ctx) override;


private:
    // Internal implementations (called by the virtual overrides above)
    void sync_emitters(Scene& scene);

public:
    /// Register a scene's emitters and resolve what their modules reference,
    /// without the per-frame bookkeeping sync() adds for the frame's scene
    /// (the file watcher). For a scene rendered outside the frame.
    void sync_scene(Scene& scene, ResourceManager* resources, const Project* project);

    /// Drop the emitters a scene owned. For a scene that is going away;
    /// one that merely lost its emitter nodes is reconciled by sync.
    void release_scene(uint64_t scene_id);

private:
    void dispatch_compute(const FrameContext& ctx, Scene& scene, const glm::vec3& wind,
                          const glm::mat4& view_projection);
    void record_render(const FrameContext& ctx);
    void record_render_distortion(const FrameContext& ctx);
    void record_render_opaque(const FrameContext& ctx);

public:
    // Public for DDGI RT TLAS integration (mesh particle emitters as proxy geometry)
    struct EmitterGpu {
        // A slot in emitters_. Slots are never erased: particles carry
        // their emitter's index for their whole life and the per-emitter
        // counters live at that index, so an emitter that goes away leaves
        // its slot free for the next one rather than shifting everything
        // above it onto the wrong data.
        bool active{false};
        // The scene the emitter's node lives in. Every scene rendered keeps
        // its emitters here; a dispatch only drives the ones of the scene
        // it is given and freezes the rest, so a preview scene rendered
        // between two frames of the main scene leaves the main scene's
        // effects exactly where they were.
        uint64_t scene_id{0};
        bool in_scene{true};        // owner is the scene of the current dispatch
        uint32_t object_id{0};
        uint32_t emitter_index{0};  // index into VfxEffect::emitters
        uint32_t max_particles{0};
        uint32_t base_offset{0};
        ParticleBlendMode blend_mode{ParticleBlendMode::additive};
        glm::vec3 prev_position{0.0f};
        bool has_prev_position{false};
        bool visible{true};       // frustum culling result
        bool is_trail{false};     // uses trail ring buffer emit + strip rendering
        bool is_lit{false};       // uses lit PBR fragment shader
        float lit_metallic{0.0f};
        float lit_roughness{0.5f};
        bool is_mesh{false};         // uses mesh instanced rendering
        bool is_distortion{false};   // renders into distortion UV-offset buffer
        float distortion_strength{0.05f};
        ParticleMeshShape mesh_shape{ParticleMeshShape::cube};
        MegaMeshEntry mesh_entry{}; // resolved mesh geometry
        uint32_t mesh_material_id{0}; // resolved material for mesh particles
    };
    [[nodiscard]] const std::vector<EmitterGpu>& emitters() const { return emitters_; }
    /// Every emitter's simulated particles; invalid before the first.
    [[nodiscard]] gpu::Buffer particle_buffer() const {
        return mega_particle_buffer_ ? mega_particle_buffer_->shared() : gpu::Buffer{};
    }

    void set_sdf_bounds(const glm::vec3& vol_min, const glm::vec3& vol_max) {
        sdf_volume_min_ = vol_min;
        sdf_volume_max_ = vol_max;
    }

private:

    void rebuild_mega_buffers(uint32_t total_particles, uint32_t emitter_count);

    // Buffer regions to clear before the next dispatch. Sync runs before
    // any command buffer is open, so what it changes is queued here and
    // recorded into the frame ahead of the simulation, rather than
    // submitted on its own and waited for: a spawn must not drain the GPU.
    struct SlotInit {
        uint32_t slot;
        uint32_t base_offset;
        uint32_t max_particles;
        bool reset_counters; // a slot taken anew; a freed one only loses its particles
    };
    std::vector<SlotInit> pending_inits_;
    /// Zero the particles and reset the counters of the given slots, at
    /// the next dispatch.
    void init_emitter_slots(const std::vector<uint32_t>& slots);
    void record_pending_inits(VkCommandBuffer cmd);
    /// A free slot in emitters_, appending one when none is free.
    [[nodiscard]] uint32_t allocate_slot();
    /// The first run of `count` particles no active emitter holds, or
    /// nullopt when there is none under the scene ceiling.
    [[nodiscard]] std::optional<uint32_t> allocate_particle_range(uint32_t count) const;
    /// The slot's particles and range are up for reuse.
    void free_slot(EmitterGpu& gpu);
    /// One past the highest particle any active emitter holds: how far the
    /// simulation has to reach and how big the buffers must be.
    [[nodiscard]] uint32_t particle_extent() const;

    VkDevice device_{VK_NULL_HANDLE};
    gpu::Device* gpu_device_{nullptr};
    VkPipelineCache vk_pipeline_cache_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};

    // Every pipeline the system draws or dispatches with, built together
    // so a shader reload that fails anywhere keeps the previous set whole.
    struct Pipelines {
        VkPipeline emit{VK_NULL_HANDLE};
        VkPipeline sim{VK_NULL_HANDLE};
        VkPipeline indirect{VK_NULL_HANDLE};
        VkPipeline mesh_instance{VK_NULL_HANDLE};   // alive-particle compaction (billboard + mesh)
        VkPipeline additive{VK_NULL_HANDLE};        // billboard, unlit
        VkPipeline alpha{VK_NULL_HANDLE};
        VkPipeline trail_additive{VK_NULL_HANDLE};  // ribbon strip, unlit
        VkPipeline trail_alpha{VK_NULL_HANDLE};
        VkPipeline lit_additive{VK_NULL_HANDLE};    // billboard, PBR
        VkPipeline lit_alpha{VK_NULL_HANDLE};
        VkPipeline lit_trail_additive{VK_NULL_HANDLE};
        VkPipeline lit_trail_alpha{VK_NULL_HANDLE};
        VkPipeline mesh_additive{VK_NULL_HANDLE};   // instanced mesh, unlit
        VkPipeline mesh_alpha{VK_NULL_HANDLE};
        VkPipeline mesh_lit_additive{VK_NULL_HANDLE};
        VkPipeline mesh_lit_alpha{VK_NULL_HANDLE};
        VkPipeline opaque{VK_NULL_HANDLE};          // depth-writing, no blend
        VkPipeline opaque_lit{VK_NULL_HANDLE};
        VkPipeline mesh_opaque{VK_NULL_HANDLE};
        VkPipeline mesh_opaque_lit{VK_NULL_HANDLE};
        VkPipeline distortion{VK_NULL_HANDLE};      // UV-offset buffer, one R16G16 attachment
    };
    Pipelines pipelines_;
    /// Build every pipeline from the shaders on disk into `out`. On any
    /// failure nothing of `out` is left alive and false is returned.
    [[nodiscard]] bool build_pipelines(Pipelines& out);
    void destroy_pipelines(Pipelines& p);

    // Compute pipeline (shared set layout for all 3 shaders)
    VkDescriptorSetLayout compute_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout compute_layout_{VK_NULL_HANDLE};

    // Pipeline layouts: unlit (no push constants), lit (metallic/roughness/
    // material push), distortion (strength push)
    VkDescriptorSetLayout render_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout render_layout_{VK_NULL_HANDLE};
    VkPipelineLayout render_distortion_layout_{VK_NULL_HANDLE};
    VkPipelineLayout render_lit_layout_{VK_NULL_HANDLE};

    // Dispatch regions. The emitter records and module bytes are written by
    // the CPU while the dispatch is recorded and read by the GPU when it
    // runs, so each dispatch gets its own copy, with descriptor sets to
    // match: earlier frames may still be executing theirs, and one frame
    // records a dispatch per viewport it renders, plus any preview drawn
    // in the window before it. A frame slot therefore owns several
    // regions, taken in turn.
    static constexpr uint32_t DISPATCHES_PER_FRAME = 4;
    static constexpr uint32_t DISPATCH_REGIONS = MAX_FRAMES_IN_FLIGHT * DISPATCHES_PER_FRAME;
    uint32_t region_{0};               // the region the current dispatch and its draws use
    uint32_t region_frame_{UINT32_MAX}; // frame slot the ordinal counts within
    uint32_t region_ordinal_{0};
    bool region_overflow_warned_{false};
    [[nodiscard]] uint32_t take_region(uint32_t frame_index);

    // Descriptor pool + per-region sets
    VkDescriptorPool pool_{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, DISPATCH_REGIONS> compute_sets_{};
    std::array<VkDescriptorSet, DISPATCH_REGIONS> render_sets_{};

    // Mega buffers. The four GPU-only ones grow via sparse binding (see
    // renderer/gpu/growable_buffer.hpp) — their handle and device address
    // never move once committed, so growth never stalls the device and
    // their descriptor bindings never need rewriting. The emitter data and
    // module buffers are HOST_VISIBLE and CPU-written every dispatch, which
    // GrowableBuffer can't back on this hardware (sparse buffers reject
    // host-visible memory outright — verified, not a missing feature to
    // add later) — they keep the older allocate/copy/retire growth path.
    std::unique_ptr<GrowableBuffer> mega_particle_buffer_;
    std::unique_ptr<GrowableBuffer> mega_counter_buffer_;
    std::unique_ptr<GrowableBuffer> mega_indirect_buffer_;
    std::unique_ptr<GrowableBuffer> mega_alive_buffer_;   // per-emitter dense alive indices
    std::array<std::unique_ptr<Buffer>, DISPATCH_REGIONS> emitter_data_buffers_; // HOST_VISIBLE, mapped
    std::array<std::unique_ptr<Buffer>, DISPATCH_REGIONS> module_buffers_;       // HOST_VISIBLE, per-emitter module data
    std::unique_ptr<Buffer> sub_event_buffer_;      // GPU-only, sub-emitter events (64 bytes each)
    std::unique_ptr<Buffer> sub_event_counter_buffer_; // GPU-only, one uint32 per emitter

    // Emitter tracking
    std::vector<EmitterGpu> emitters_;
    std::unordered_set<uint32_t> emitter_id_set_;
    std::vector<GpuEmitterData> emitter_data_cache_;

    // Capacity
    uint32_t total_particle_capacity_{0};
    uint32_t emitter_capacity_{0};

    // Cached layouts
    VkDescriptorSetLayout global_set_layout_{VK_NULL_HANDLE};
    VkFormat color_format_{VK_FORMAT_UNDEFINED};
    VkFormat depth_format_{VK_FORMAT_UNDEFINED};
    VkSampleCountFlagBits sample_count_{VK_SAMPLE_COUNT_1_BIT};
    VkSampler depth_sampler_{VK_NULL_HANDLE};
    VkDescriptorSetLayout bindless_set_layout_{VK_NULL_HANDLE};

    // Opaque pipeline layouts (same sets; the lit one carries the push range)
    VkPipelineLayout render_opaque_layout_{VK_NULL_HANDLE};
    VkPipelineLayout render_opaque_lit_layout_{VK_NULL_HANDLE};

    // ── Mesh particle rendering ──
    // Instance data written by compute, read by mesh vertex shader
    struct GpuMeshInstance {
        glm::mat4 model;          // 64
        glm::vec4 color;          // 16
        uint32_t texture_index;   // 4
        uint32_t pad[3];          // 12
    };
    static_assert(sizeof(GpuMeshInstance) == 96);

    std::unique_ptr<Buffer> mesh_instance_buffer_;
    std::unique_ptr<Buffer> mesh_indirect_buffer_;
    std::unique_ptr<Buffer> mesh_instance_count_buffer_; // atomic counter per mesh emitter

    VkDescriptorSetLayout mesh_compute_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout mesh_compute_layout_{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, DISPATCH_REGIONS> mesh_compute_sets_{};


    // Built-in primitive meshes
    MegaMeshEntry mesh_cube_{};
    MegaMeshEntry mesh_sphere_{};
    bool primitives_uploaded_{false};

    void upload_primitives(MegaBuffer& mega_buffer);

    uint32_t frame_seed_{0};
    float accumulated_time_{0.0f};
    glm::vec3 camera_pos_{0.0f};
    glm::mat4 cached_view_projection_{1.0f};

    glm::vec3 sdf_volume_min_{-10.0f};
    glm::vec3 sdf_volume_max_{10.0f};

#ifndef NDEBUG
    platform::FileWatcher vfx_watcher_;
    const Scene* watched_scene_{nullptr};
    bool vfx_changed_{false};
    std::string vfx_changed_path_;
    std::string vfx_relative_path_;
    std::unordered_set<const Scene*> vfx_reloaded_scenes_;
    void setup_vfx_watcher(const RenderSubsystemContext& ctx);
    void reload_changed_vfx(const RenderSubsystemContext& ctx);
#endif
};

} // namespace fjell
