#pragma once

#include "core/result.hpp"
#include "renderer/frame_graph.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/render_pipeline.hpp"

#include <array>
#include <functional>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

namespace fjell {

struct DrawCommand;
struct EnvironmentState;
struct RenderStats;
class Device;
class DebugDraw;
class DeferredDeleter;
class GpuContext;
class OffscreenPass;
struct SkyBackdrop;
class PassRegistry;
class RenderQueue;
class RenderSubsystem;
class RenderTargetPool;
class ResourceRegistry;
class Scene;
class UploadContext;

// Shared descriptor set layouts needed by passes at pipeline creation time.
struct PassResources {
    VkDescriptorSetLayout global_set_layout{VK_NULL_HANDLE};
    VkDescriptorSetLayout material_set_layout{VK_NULL_HANDLE};
    VkDescriptorSetLayout draw_data_layout{VK_NULL_HANDLE};
    VkDescriptorSetLayout meshlet_layout{VK_NULL_HANDLE};
    VkFormat depth_format{VK_FORMAT_UNDEFINED};
    VkFormat color_format{VK_FORMAT_UNDEFINED};
    VkSampleCountFlagBits sample_count{VK_SAMPLE_COUNT_1_BIT};
    VmaAllocator allocator{VK_NULL_HANDLE};
    VkPipelineCache pipeline_cache{VK_NULL_HANDLE};
    VkCommandPool command_pool{VK_NULL_HANDLE};
    VkQueue graphics_queue{VK_NULL_HANDLE};
    // Needed by subsystems that own a GrowableBuffer (sparse-binding GPU
    // buffer growth — see renderer/gpu/growable_buffer.hpp).
    UploadContext* upload_context{nullptr};
    DeferredDeleter* deferred_deleter{nullptr};
};

class FrameDescriptorCache;
class ThreadPool;
class IndirectBatch;
class MegaBuffer;
class PipelineCache;
class PostProcessResources;
class TransientImagePool;

// Shared state that every pass can read during recording.
struct FrameContext {
    VkCommandBuffer cmd{VK_NULL_HANDLE};
    // Secondary command buffer for async-hinted passes. Null when the
    // device doesn't expose a compute queue family, or for viewport
    // previews that share the main frame's submission. FrameGraph::execute
    // routes compute passes here; a null handle falls back to `cmd`.
    VkCommandBuffer async_compute_cmd{VK_NULL_HANDLE};
    // Second graphics CB — post-compute graphics work records into this
    // one when the frame uses async compute. FrameGraph picks between
    // this and `cmd` based on DAG position relative to the first async
    // pass. Null falls back to `cmd` (same as today's single-CB flow).
    VkCommandBuffer graphics_post_cmd{VK_NULL_HANDLE};
    OffscreenPass* target{nullptr};
    const RenderQueue* queue{nullptr};
    VkDescriptorSet global_descriptor_set{VK_NULL_HANDLE};
    VkDescriptorSet bindless_set{VK_NULL_HANDLE};
    VkDescriptorSet draw_data_set{VK_NULL_HANDLE};
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
    /// The engine's worker threads, for a pass with CPU work too slow for the
    /// frame it is asked in. Set for prepare_frame; null elsewhere.
    ThreadPool* thread_pool{nullptr};
    uint32_t selected_id{0};
    bool show_colliders{false};
    uint32_t frame_index{0};
    IndirectBatch* indirect_batch{nullptr};
    MegaBuffer* mega_buffer{nullptr};
    VkDescriptorSet meshlet_set{VK_NULL_HANDLE};
    PipelineCache* pipeline_cache{nullptr};

    // Two-pass occlusion: GPU-driven dispatch from culled visible buffer
    VkBuffer gpu_driven_indirect{VK_NULL_HANDLE}; // meshlet_instance_pass indirect cmds
    uint32_t gpu_driven_segment_size{0};           // instances per pipeline bin

    // Transient render target pool
    RenderTargetPool* target_pool{nullptr};

    // Per-viewport post-process images
    PostProcessResources* pp_resources{nullptr};

    // Per-frame descriptor set cache; passes call acquire() to get a set
    // bound to transient views without managing a pool themselves.
    FrameDescriptorCache* descriptor_cache{nullptr};

    // Cross-frame VMA-backed image pool for FrameGraph's aliased transients.
    TransientImagePool* transient_pool{nullptr};

    // Named resource registry (pass-to-pass data flow)
    ResourceRegistry* registry{nullptr};

    // Inverse view-projection for world-space reconstruction (DDGI sampling)
    glm::mat4 inv_view_proj{1.0f};

    // Skip shadow pass for this viewport (e.g. camera preview uses main viewport's shadows)
    bool skip_shadows{false};

    // Flip tonemap output vertically so render-to-texture viewports produce
    // images with V=0 at bottom (standard texture convention).
    bool flip_tonemap_y{false};
    // Drawn in place of the scene's sky when set (see ViewportRenderRequest).
    const SkyBackdrop* backdrop{nullptr};

    // ── Extended context (for self-registering passes) ─────────────────

    // Render statistics (draw calls, triangles, etc.)
    RenderStats* render_stats{nullptr};

    // Tracy GPU profiling context (void* to avoid header dependency)
    void* tracy_ctx{nullptr};

    // Mesh shader indirect dispatch function pointer (null if unsupported)
    PFN_vkCmdDrawMeshTasksIndirectEXT draw_mesh_tasks_indirect_fn{nullptr};

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
    // pass). Screen-space UI no longer has a FrameContext-level callback —
    // it goes through PlayController's own HUD target and VulkanContext's
    // play_hud_callback_, invoked from record_command_buffer.
    using UiCallback = std::function<void(VkCommandBuffer, VkDescriptorSet,
                                          VkDescriptorSet, VkExtent2D, uint32_t)>;
    UiCallback world_ui_callback;

    // Per-cascade shadow batches (indexed by cascade index)
    static constexpr uint32_t MAX_SHADOW_CASCADES = 3;
    std::array<IndirectBatch*, MAX_SHADOW_CASCADES> shadow_cascade_batches{};
    std::array<VkDescriptorSet, MAX_SHADOW_CASCADES> shadow_cascade_draw_data{};

    std::array<const std::vector<DrawCommand>*, MAX_SHADOW_CASCADES> shadow_cascade_commands{};
};

/// How a pass prefers to be scheduled relative to the graphics queue.
/// Phase 3 (`RenderPipeline` dual-queue dispatch) reads this; Phase 2
/// only populates it. When `Device::async_compute_supported()` is false
/// every pass degrades to `None` regardless of the hint.
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
    [[nodiscard]] virtual Result<> create(Device& device, const PassResources& resources) = 0;

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

    [[nodiscard]] Result<> recreate(Device& device, const PassResources& resources) {
        destroy();
        return create(device, resources);
    }
};

} // namespace fjell
