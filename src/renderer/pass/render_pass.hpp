#pragma once

#include "core/result.hpp"
#include "gpu/command_list.hpp"
#include "gpu/pipeline.hpp"
#include "renderer/frame_graph.hpp"
#include "renderer/declare_context.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/render_pipeline.hpp"
#include "renderer/resource_registry.hpp"

#include <array>
#include <functional>
#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace fjell {

struct DrawCommand;
struct EnvironmentState;
struct RenderStats;
class DebugDraw;
class GpuContext;
class OffscreenPass;
struct SkyBackdrop;
class PassRegistry;
class RenderQueue;
class RenderSubsystem;
class ResourceRegistry;
class Scene;

// What a pass needs to make its pipelines: the shared layouts, the scene
// targets' formats and sample count, and the device.
struct PassResources {
    /// What a pipeline names
    /// in `.shared` to bind the engine's globals, bindless table, draw data
    /// and meshlets.
    gpu::SharedLayout globals{};
    gpu::SharedLayout bindless{};
    gpu::SharedLayout draw_data{};
    gpu::SharedLayout meshlets{};
    /// The GPU interface's device, for what a pass makes through it or hands
    /// it: an image made before the interface is adopted where it is made.
    gpu::Device* device{nullptr};
    /// The scene colour's and depth's formats as the GPU interface names
    /// them, for pipelines it builds.
    gpu::Format scene_color_format{gpu::Format::undefined};
    gpu::Format scene_depth_format{gpu::Format::undefined};
    /// The scene's other targets, which surfaces write beside colour:
    /// metallic/roughness, normal and motion.
    gpu::Format scene_mr_format{gpu::Format::undefined};
    gpu::Format scene_normal_format{gpu::Format::undefined};
    gpu::Format scene_velocity_format{gpu::Format::undefined};
    /// The single-sampled target distortion draws its screen offsets into.
    gpu::Format scene_distortion_format{gpu::Format::undefined};
    gpu::Samples samples{gpu::Samples::x1};
};

class ThreadPool;
class IndirectBatch;
class MegaBuffer;
class PipelineCache;
class PostProcessResources;
class TransientImagePool;

// Shared state that every pass can read during recording.
struct FrameContext {
    /// The pass's commands.
    [[nodiscard]] gpu::CommandList& commands() const { return *command_list; }
    /// Set by the frame graph for each pass it records; read through
    /// `commands()`.
    gpu::CommandList* command_list{nullptr};
    /// The GPU interface's device.
    gpu::Device* device{nullptr};
    /// The frame's compute list, for the passes hinted to run on the
    /// compute queue; none on a device without one, or for a view other
    /// than the frame's main one, whose passes all record into one list.
    gpu::CommandList* async_compute{nullptr};
    /// The graphics list after the compute list, for what comes after the
    /// first compute pass; none where there is no compute list.
    gpu::CommandList* graphics_post{nullptr};
    OffscreenPass* target{nullptr};
    const RenderQueue* queue{nullptr};
    /// The view's global set, for pipelines that name the shared globals
    /// layout.
    gpu::BindGroup globals{};
    /// What that set binds at the GI slots this run: DDGI's and SS-GI's
    /// outputs once they hold something, null while it binds the fallback.
    gpu::Texture bound_gi_indirect{};
    gpu::Texture bound_ss_gi_emissive{};
    /// The bindless table, for pipelines that name the shared bindless
    /// layout.
    gpu::BindGroup bindless{};
    /// The draw data group, for pipelines that name the shared draw data
    /// layout.
    gpu::BindGroup draw_data{};
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    glm::vec3 camera_position{0.0f};
    DebugDraw* debug_draw{nullptr};
    const Scene* scene{nullptr};
    // The scene's resolved environment: sun, sky, fog, shafts. Passes read
    // their lighting and atmosphere from here, never from the scene's nodes.
    const EnvironmentState* environment{nullptr};
    float delta_time{0.0f};
    /// Seconds the renderer has been drawing for: the `time` shaders read from
    /// the global UBO, for a pass that has to run on the same clock as one.
    float elapsed_time{0.0f};
    uint32_t selected_id{0};
    bool show_colliders{false};
    /// The viewport being drawn asks for the editor's ground grid.
    bool show_grid{false};
    uint32_t frame_index{0};
    IndirectBatch* indirect_batch{nullptr};
    MegaBuffer* mega_buffer{nullptr};
    /// The meshlet group; unset when this viewport's graph runs no meshlet
    /// cull.
    gpu::BindGroup meshlets{};
    PipelineCache* pipeline_cache{nullptr};

    /// The meshlet cull's commands, one gpu::DrawMeshTasksArgs per pipeline
    /// bin, drawn through MeshletInstancePass::draw_bin(). Unset when the
    /// viewport's graph runs no meshlet cull.
    gpu::Buffer gpu_driven_commands{};

    // Per-viewport post-process images
    PostProcessResources* pp_resources{nullptr};

    // Cross-frame VMA-backed image pool for FrameGraph's aliased transients.
    TransientImagePool* transient_pool{nullptr};

    // Named resource registry (pass-to-pass data flow)
    ResourceRegistry* registry{nullptr};
    /// The texture this viewport's graph knows by `name`: a target image or a
    /// transient the graph made. Invalid when there is none.
    [[nodiscard]] gpu::Texture texture(const std::string& name) const {
        return registry != nullptr ? registry->texture(name) : gpu::Texture{};
    }

    // Inverse view-projection for world-space reconstruction (DDGI sampling)
    glm::mat4 inv_view_proj{1.0f};

    // Skip shadow pass for this viewport (e.g. camera preview uses main viewport's shadows)
    bool skip_shadows{false};

    // Drawn in place of the scene's sky when set (see ViewportRenderRequest).
    const SkyBackdrop* backdrop{nullptr};

    // ── Extended context (for self-registering passes) ─────────────────

    // Render statistics (draw calls, triangles, etc.)
    RenderStats* render_stats{nullptr};

    // Render subsystems (particles, sprites, tilemaps).
    // Points into VulkanContext's cached raw-pointer vector.
    RenderSubsystem* const* subsystems{nullptr};
    uint32_t subsystem_count{0};

    // Debug viewport bitfield (which viz passes are active)
    uint32_t active_sources{0};

    // Pass registry for cross-pass record calls (e.g. terrain from opaque)
    PassRegistry* passes{nullptr};

    // Game state for UI rendering
    bool playing{false};

    // Worldspace UI rendering callback (set by game layer, used by transparent
    // pass): the pass's encoder, the global and bindless groups, the target's
    // size and the frame slot. Screen-space UI no longer has a
    // FrameContext-level callback — it goes through PlayController's own HUD
    // target.
    using UiCallback = std::function<void(gpu::RenderEncoder&, gpu::BindGroup, gpu::BindGroup,
                                          glm::uvec2, uint32_t)>;
    UiCallback world_ui_callback;

    // Per-cascade shadow batches (indexed by cascade index)
    static constexpr uint32_t MAX_SHADOW_CASCADES = 3;
    std::array<IndirectBatch*, MAX_SHADOW_CASCADES> shadow_cascade_batches{};
    /// Each cascade's draw data group.
    std::array<gpu::BindGroup, MAX_SHADOW_CASCADES> shadow_cascade_draw_data_groups{};

    std::array<const std::vector<DrawCommand>*, MAX_SHADOW_CASCADES> shadow_cascade_commands{};
};

/// How a pass prefers to be scheduled relative to the graphics queue.
/// Phase 3 (`RenderPipeline` dual-queue dispatch) reads this; Phase 2
/// only populates it. When the device has no async compute every pass
/// degrades to `None` regardless of the hint.
enum class AsyncComputeHint : uint8_t {
    /// Must run on the graphics queue — typically because the pass uses
    /// render targets or records commands that aren't valid on a pure
    /// compute queue (begin/end rendering, graphics pipelines, blits).
    None,

    /// Can run on the compute queue when the hardware supports async.
    /// Falls back to graphics otherwise. Use this for compute passes
    /// whose outputs can comfortably survive one timeline hop before
    /// downstream graphics work consumes them (GTAO, light cull, DDGI).
    Prefer,
};

/// Pipeline registration metadata: pass name, execution phase, sub_order,
/// and async-compute hint. `phase` + `sub_order` drive the deterministic
/// tiebreaker during the DAG topological sort; `async_hint` advises the
/// pipeline on queue routing. Resource reads/writes are described
/// through declare()/PassBuilder instead.
struct PassSetup {
    std::string name;
    RenderPhase phase{RenderPhase::Color};
    uint32_t sub_order{0};
    AsyncComputeHint async_hint{AsyncComputeHint::None};
};

// Base class for a render pass. Each pass owns its pipeline(s) and
// knows how to record its commands into a command buffer.
class RenderPass {
public:
    virtual ~RenderPass() = default;

    RenderPass() = default;
    RenderPass(const RenderPass&) = delete;
    RenderPass& operator=(const RenderPass&) = delete;
    RenderPass(RenderPass&&) = delete;
    RenderPass& operator=(RenderPass&&) = delete;

    // Create GPU resources (pipelines, layouts).
    [[nodiscard]] virtual Result<> create(const PassResources& resources) = 0;

    // Destroy GPU resources.
    virtual void destroy() = 0;

    /// Post-creation hook called after all passes are created.
    /// Override to perform one-shot GPU work (cubemap generation),
    /// write global descriptors, or link to sibling passes.
    virtual void post_create(GpuContext& gpu, PassRegistry& passes) {
        (void)gpu; (void)passes;
    }

    /// Once-per-frame hook called before any viewport is rendered.
    /// Passes that need scene-driven state (environment nodes, light
    /// direction, fog params) override this to scan the scene graph
    /// and configure themselves. Runs before gather(), so shadow
    /// cascade matrices are available for frustum culling.
    virtual void prepare_frame(const FrameContext& /*ctx*/, GpuContext& /*gpu*/) {}

    /// Register persistent resources (images, buffers) into the registry.
    /// Called each frame after registry clear, before pipeline execution.
    /// Override in passes that produce resources consumed by other passes.
    virtual void register_resources(ResourceRegistry& /*registry*/) {}

    /// Per-viewport setup before pipeline execution. Passes pull what
    /// they need from the context (resize buffers, upload data, build
    /// accel structures). Called after FrameContext is built, before
    /// execute().
    virtual void pre_record(const FrameContext& /*ctx*/, GpuContext& /*gpu*/) {}

    /// Per-frame cleanup after pipeline execution (e.g. copy history buffers).
    virtual void post_record(const FrameContext& /*ctx*/) {}

    // Record draw commands into the command buffer.
    virtual void record(const FrameContext& ctx) = 0;

    /// Pipeline registration metadata (name, phase, sub_order). The
    /// pipeline uses this only as the deterministic tiebreaker for the
    /// DAG topological sort.
    [[nodiscard]] virtual PassSetup setup() const { return {}; }

    /// Declare the pass's DAG topology: imported and created resources,
    /// reads, writes, queue assignment. The graph consumes this to
    /// schedule passes, allocate transient resources, and emit barriers.
    ///
    /// Must not record commands or touch per-frame transient state:
    /// called during graph construction, before any command buffer
    /// exists.
    virtual void declare(PassBuilder& /*builder*/, const DeclareContext& /*ctx*/) {}

    /// Register persistent images this pass owns and exposes to other
    /// passes under a stable name. The pipeline calls this on every pass
    /// before any declare() runs, building up the DeclareContext's
    /// named-image catalog. Consumers then reach these by name through
    /// PassBuilder::import_named() — no direct coupling to the producer
    /// class. Shadow atlases, Hi-Z pyramids, sky cubemaps, etc. go here.
    virtual void collect_exports(DeclareContext& /*ctx*/) {}

    [[nodiscard]] Result<> recreate(const PassResources& resources) {
        destroy();
        return create(resources);
    }
};

} // namespace fjell
