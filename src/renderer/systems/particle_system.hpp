#pragma once

#include "renderer/systems/particle_budget.hpp"

#include "ftk/gpu/buffer.hpp"
#include "ftk/gpu/owned.hpp"
#include "ftk/gpu/pipeline.hpp"
#include "ftk/gpu/sampler.hpp"
#include "renderer/components/particle_emitter.hpp"
#include "renderer/frustum.hpp"
#include "renderer/pass/render_pass.hpp"
#include "renderer/systems/render_subsystem.hpp"

#ifndef NDEBUG
#include "ftk/platform/file_watcher.hpp"
#endif
#include "renderer/resources/mega_buffer.hpp"
#include "renderer/systems/vfx_module.hpp"

#include <glm/glm.hpp>

#include "renderer/renderer_constants.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
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

class Scene;

class ParticleSystem : public RenderSubsystem {
public:
    void create(const PassResources& resources) override;
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
    void record_opaque(const FrameContext& ctx, ftk::gpu::RenderEncoder& pass) override;
    void record_distortion(const FrameContext& ctx, ftk::gpu::RenderEncoder& pass) override;
    void record_transparent(const FrameContext& ctx, ftk::gpu::RenderEncoder& pass) override;

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

    /// Moves the scene's effects on by `dt` seconds, once per tick, as
    /// physics is stepped: the next graph run drawing the scene with a
    /// subsystem compute pass records the step, and every other view draws
    /// what it left. A second call before any run steps replaces the first,
    /// so a scene nothing draws holds still.
    void advance(uint64_t scene_id, float dt);

private:
    // A view a scene was drawn from: what it sees and where it stands.
    struct SceneView {
        Frustum frustum;
        glm::vec3 camera{0.0f};
    };

    void dispatch_compute(const FrameContext& ctx, Scene& scene, const glm::vec3& wind, float dt,
                          std::span<const SceneView> views);
    void record_render(const FrameContext& ctx, ftk::gpu::RenderEncoder& pass);
    void record_render_distortion(const FrameContext& ctx, ftk::gpu::RenderEncoder& pass);
    void record_render_opaque(const FrameContext& ctx, ftk::gpu::RenderEncoder& pass);
    /// Sets `pipeline` and binds what its shaders read: the globals, the
    /// bindless table, and of the particle buffers those it declares.
    /// `records` holds the emitter records, which only the trails read.
    void use(const FrameContext& ctx, ftk::gpu::RenderEncoder& pass, ftk::gpu::GraphicsPipeline pipeline,
             const ftk::gpu::BufferRange& records = {});

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
        // The last step of its scene found its node and moved it on, and
        // where its particles can be as of that step, which each view culls
        // its draws by.
        bool stepped{false};
        AABB bounds{};
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
    [[nodiscard]] ftk::gpu::Buffer particle_buffer() const { return particles_; }

    void set_sdf_bounds(const glm::vec3& vol_min, const glm::vec3& vol_max) {
        sdf_volume_min_ = vol_min;
        sdf_volume_max_ = vol_max;
    }

private:

    /// Grows the buffers to hold `total_particles` particles and
    /// `emitter_count` emitters.
    void grow_buffers(uint32_t total_particles, uint32_t emitter_count);

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
    void record_pending_inits(ftk::gpu::CommandList& cmd);
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

    ftk::gpu::Device* gpu_device_{nullptr};

    // Every pipeline the system draws or dispatches with, built together
    // so a shader reload that fails anywhere keeps the previous set whole.
    struct Pipelines {
        ftk::gpu::Owned<ftk::gpu::ComputePipeline> emit;
        ftk::gpu::Owned<ftk::gpu::ComputePipeline> sim;
        ftk::gpu::Owned<ftk::gpu::ComputePipeline> indirect;
        ftk::gpu::Owned<ftk::gpu::ComputePipeline> compact;   // alive-particle compaction (billboard + mesh)
        ftk::gpu::Owned<ftk::gpu::GraphicsPipeline> additive; // billboard, unlit
        ftk::gpu::Owned<ftk::gpu::GraphicsPipeline> alpha;
        ftk::gpu::Owned<ftk::gpu::GraphicsPipeline> trail_additive; // ribbon strip, unlit
        ftk::gpu::Owned<ftk::gpu::GraphicsPipeline> trail_alpha;
        ftk::gpu::Owned<ftk::gpu::GraphicsPipeline> lit_additive; // billboard, PBR
        ftk::gpu::Owned<ftk::gpu::GraphicsPipeline> lit_alpha;
        ftk::gpu::Owned<ftk::gpu::GraphicsPipeline> lit_trail_additive;
        ftk::gpu::Owned<ftk::gpu::GraphicsPipeline> lit_trail_alpha;
        ftk::gpu::Owned<ftk::gpu::GraphicsPipeline> mesh_additive; // instanced mesh, unlit
        ftk::gpu::Owned<ftk::gpu::GraphicsPipeline> mesh_alpha;
        ftk::gpu::Owned<ftk::gpu::GraphicsPipeline> mesh_lit_additive;
        ftk::gpu::Owned<ftk::gpu::GraphicsPipeline> mesh_lit_alpha;
        ftk::gpu::Owned<ftk::gpu::GraphicsPipeline> opaque;   // depth-writing, no blend
        ftk::gpu::Owned<ftk::gpu::GraphicsPipeline> opaque_lit;
        ftk::gpu::Owned<ftk::gpu::GraphicsPipeline> mesh_opaque;
        ftk::gpu::Owned<ftk::gpu::GraphicsPipeline> mesh_opaque_lit;
        ftk::gpu::Owned<ftk::gpu::GraphicsPipeline> distortion; // screen offsets, one R16G16 target
    };
    Pipelines pipelines_;
    /// Build every pipeline from the shaders on disk into `out`. On any
    /// failure nothing is kept and false is returned.
    [[nodiscard]] bool build_pipelines(Pipelines& out);

    // What the pipelines are made for: the scene targets, the distortion
    // target, and the shared layouts they name.
    PassResources targets_{};
    ftk::gpu::Sampler depth_sampler_{};

    // The buffers the simulation keeps from frame to frame, made with room
    // to grow to the scene budgets in place: their handles never change, so
    // nothing bound to them is rewritten when they grow.
    ftk::gpu::Owned<ftk::gpu::Buffer> particles_;
    ftk::gpu::Owned<ftk::gpu::Buffer> counters_;       // per emitter: alive, dead, trail head
    ftk::gpu::Owned<ftk::gpu::Buffer> indirect_;       // per emitter: the billboard or trail draw
    ftk::gpu::Owned<ftk::gpu::Buffer> alive_;          // per emitter: dense alive indices
    ftk::gpu::Owned<ftk::gpu::Buffer> sub_events_;     // sub-emitter events, 64 bytes each
    ftk::gpu::Owned<ftk::gpu::Buffer> sub_event_counts_; // one per emitter
    ftk::gpu::Owned<ftk::gpu::Buffer> mesh_instances_; // written by compaction, read by the mesh draws
    ftk::gpu::Owned<ftk::gpu::Buffer> mesh_instance_counts_;
    ftk::gpu::Owned<ftk::gpu::Buffer> mesh_indirect_;  // per emitter: the indexed mesh draw

    /// Each emitter's module bytes as the dispatch gathers them.
    std::vector<std::byte> module_bytes_;

    // Emitter tracking
    std::vector<EmitterGpu> emitters_;
    std::unordered_set<uint32_t> emitter_id_set_;
    /// The records a step sends, one per slot: an emitter of another scene
    /// gets one that holds it still.
    std::vector<GpuEmitterData> step_records_;
    /// Each slot's record as its own scene's last step wrote it, which the
    /// draws read: another scene's step leaves it alone.
    std::vector<GpuEmitterData> draw_records_;

    // Per scene the engine advances: the step waiting to be recorded, and
    // the views that drew the scene this tick and the tick before, which
    // decide where it emits.
    struct SceneClock {
        float dt{0.0f};
        bool pending{false};
        std::vector<SceneView> views;
        std::vector<SceneView> last_views;
    };
    std::unordered_map<uint64_t, SceneClock> clocks_;

    // Capacity
    uint32_t total_particle_capacity_{0};
    uint32_t emitter_capacity_{0};

    // ── Mesh particle rendering ──
    // Instance data written by compute, read by mesh vertex shader
    struct GpuMeshInstance {
        glm::mat4 model;          // 64
        glm::vec4 color;          // 16
        uint32_t texture_index;   // 4
        uint32_t pad[3];          // 12
    };
    static_assert(sizeof(GpuMeshInstance) == 96);

    // Built-in primitive meshes
    MegaMeshEntry mesh_cube_{};
    MegaMeshEntry mesh_sphere_{};
    bool primitives_uploaded_{false};

    void upload_primitives(MegaBuffer& mega_buffer);

    uint32_t frame_seed_{0};
    float accumulated_time_{0.0f};

    glm::vec3 sdf_volume_min_{-10.0f};
    glm::vec3 sdf_volume_max_{10.0f};

#ifndef NDEBUG
    ftk::platform::FileWatcher vfx_watcher_;
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
