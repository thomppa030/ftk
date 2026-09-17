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
#include <unordered_set>
#include <vector>

namespace fjell {

// GPU-side emitter metadata (160 bytes, matches GLSL EmitterData)
struct GpuEmitterData {
    glm::vec3 emitter_position; float dt;
    glm::vec3 gravity;          uint32_t max_particles;
    glm::vec4 color_start;
    glm::vec4 color_end;
    float size_start, size_end, lifetime_min, lifetime_max;
    glm::vec3 velocity_min;     uint32_t emit_count;
    glm::vec3 velocity_max;     uint32_t random_seed;
    uint32_t base_offset;
    uint32_t counter_offset;
    uint32_t dead_list_offset;
    uint32_t module_offset;       // offset into module buffer (in uint32_t units)
    glm::vec3 emitter_velocity; float _pad0;
    uint32_t flags;               // bit 0: is_trail emitter
    uint32_t trail_head;          // ring buffer write index (from counter buffer)
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
    // Explicit tail padding: std430 rounds an array's per-element stride up
    // to the alignment of its largest member (16, from the vec3/vec4
    // fields above), so the GPU reads this struct at 192-byte intervals
    // regardless of its own packed size. Without this, C++ writes at 188
    // and the GPU reads at 192 — a silent 4-byte drift per emitter that
    // corrupts every emitter past index 0.
    uint32_t _pad1;
};
static_assert(sizeof(GpuEmitterData) == 192);

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

class DeferredDeleter;
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
    void dispatch_compute(VkCommandBuffer cmd, float dt, Scene& scene, const glm::vec3& wind,
                          VkDescriptorSet global_set,
                          const glm::vec3& camera_pos,
                          const glm::mat4& view_projection);
    void insert_barriers(VkCommandBuffer cmd);
    void record_render(const FrameContext& ctx);
    void record_render_distortion(const FrameContext& ctx);
    void record_render_opaque(const FrameContext& ctx);

public:
    // Public for DDGI RT TLAS integration (mesh particle emitters as proxy geometry)
    struct EmitterGpu {
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
    [[nodiscard]] VkBuffer particle_buffer_handle() const {
        return mega_particle_buffer_ ? mega_particle_buffer_->handle() : VK_NULL_HANDLE;
    }

    void set_sdf_bounds(const glm::vec3& vol_min, const glm::vec3& vol_max) {
        sdf_volume_min_ = vol_min;
        sdf_volume_max_ = vol_max;
    }

private:

    void rebuild_mega_buffers(uint32_t total_particles, uint32_t emitter_count);
    void init_emitter_ranges();
    void init_emitter_range(uint32_t from, uint32_t to);

    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineCache vk_pipeline_cache_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};
    VkCommandPool command_pool_{VK_NULL_HANDLE};
    VkQueue queue_{VK_NULL_HANDLE};
    DeferredDeleter* deleter_{nullptr}; // retires replaced buffers instead of
                                        // freeing them while a frame in
                                        // flight might still be reading them

    // Compute pipeline (shared set layout for all 3 shaders)
    VkDescriptorSetLayout compute_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout compute_layout_{VK_NULL_HANDLE};
    VkPipeline emit_pipeline_{VK_NULL_HANDLE};
    VkPipeline sim_pipeline_{VK_NULL_HANDLE};
    VkPipeline indirect_pipeline_{VK_NULL_HANDLE};

    // Render pipeline (billboard)
    VkDescriptorSetLayout render_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout render_layout_{VK_NULL_HANDLE};
    VkPipeline render_additive_{VK_NULL_HANDLE};
    VkPipeline render_alpha_{VK_NULL_HANDLE};

    // Render pipeline (trail / ribbon)
    VkPipeline render_trail_additive_{VK_NULL_HANDLE};
    VkPipeline render_trail_alpha_{VK_NULL_HANDLE};

    // Distortion render pipeline (single R16G16 attachment, 1x MSAA)
    VkPipelineLayout render_distortion_layout_{VK_NULL_HANDLE};
    VkPipeline render_distortion_{VK_NULL_HANDLE};

    // Lit render pipelines (separate layout with push constants)
    VkPipelineLayout render_lit_layout_{VK_NULL_HANDLE};
    VkPipeline render_lit_additive_{VK_NULL_HANDLE};
    VkPipeline render_lit_alpha_{VK_NULL_HANDLE};
    VkPipeline render_lit_trail_additive_{VK_NULL_HANDLE};
    VkPipeline render_lit_trail_alpha_{VK_NULL_HANDLE};

    // Descriptor pool + single sets
    VkDescriptorPool pool_{VK_NULL_HANDLE};
    VkDescriptorSet compute_set_{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> render_sets_{};

    // Mega buffers. The four GPU-only ones grow via sparse binding (see
    // renderer/gpu/growable_buffer.hpp) — their handle and device address
    // never move once committed, so growth never stalls the device and
    // their descriptor bindings never need rewriting. emitter_data_/
    // module_buffer_ are HOST_VISIBLE and CPU-written every frame, which
    // GrowableBuffer can't back on this hardware (sparse buffers reject
    // host-visible memory outright — verified, not a missing feature to
    // add later) — they keep the older allocate/copy/retire growth path.
    std::unique_ptr<GrowableBuffer> mega_particle_buffer_;
    std::unique_ptr<GrowableBuffer> mega_counter_buffer_;
    std::unique_ptr<GrowableBuffer> mega_dead_list_buffer_;
    std::unique_ptr<GrowableBuffer> mega_indirect_buffer_;
    std::unique_ptr<Buffer> emitter_data_buffer_;  // HOST_VISIBLE, mapped
    std::unique_ptr<Buffer> module_buffer_;         // HOST_VISIBLE, per-emitter module data
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

    // Opaque render pipelines (depth write, back-face cull)
    VkPipeline render_opaque_{VK_NULL_HANDLE};          // unlit billboard opaque
    VkPipeline render_opaque_lit_{VK_NULL_HANDLE};      // lit billboard opaque
    VkPipeline render_mesh_opaque_{VK_NULL_HANDLE};     // unlit mesh opaque
    VkPipeline render_mesh_opaque_lit_{VK_NULL_HANDLE}; // lit mesh opaque
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

    VkPipeline mesh_instance_pipeline_{VK_NULL_HANDLE};
    VkDescriptorSetLayout mesh_compute_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout mesh_compute_layout_{VK_NULL_HANDLE};
    VkDescriptorSet mesh_compute_set_{VK_NULL_HANDLE};

    // Mesh render pipelines (unlit + lit) × (additive + alpha) = 4
    VkPipeline render_mesh_additive_{VK_NULL_HANDLE};
    VkPipeline render_mesh_alpha_{VK_NULL_HANDLE};
    VkPipeline render_mesh_lit_additive_{VK_NULL_HANDLE};
    VkPipeline render_mesh_lit_alpha_{VK_NULL_HANDLE};
    VkPipelineLayout mesh_render_layout_{VK_NULL_HANDLE};
    VkPipelineLayout mesh_render_lit_layout_{VK_NULL_HANDLE};

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
    std::shared_ptr<VfxEffect> vfx_pending_effect_;
    std::unordered_set<const Scene*> vfx_reloaded_scenes_;
    void setup_vfx_watcher(const RenderSubsystemContext& ctx);
    void reload_changed_vfx(const RenderSubsystemContext& ctx);
#endif
};

} // namespace fjell
