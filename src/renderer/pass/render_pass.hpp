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
struct RenderStats;
class Device;
class DebugDraw;
class GpuContext;
class OffscreenPass;
class PassRegistry;
class RenderQueue;
class RenderSubsystem;
class RenderTargetPool;
class ResourceRegistry;
class Scene;

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
};

class IndirectBatch;
class MegaBuffer;
class PipelineCache;
class PostProcessResources;

// Shared state that every pass can read during recording.
struct FrameContext {
    VkCommandBuffer cmd{VK_NULL_HANDLE};
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
    float delta_time{0.0f};
    uint32_t selected_id{0};
    bool show_colliders{false};
    uint32_t frame_index{0};
    IndirectBatch* indirect_batch{nullptr};
    MegaBuffer* mega_buffer{nullptr};
    VkDescriptorSet meshlet_set{VK_NULL_HANDLE};
    bool use_mesh_shaders{false};
    PipelineCache* pipeline_cache{nullptr};

    // Two-pass occlusion: GPU-driven dispatch from culled visible buffer
    VkBuffer gpu_driven_indirect{VK_NULL_HANDLE}; // meshlet_instance_pass indirect cmds
    uint32_t gpu_driven_segment_size{0};           // instances per pipeline bin

    // Transient render target pool
    RenderTargetPool* target_pool{nullptr};

    // Per-viewport post-process images
    PostProcessResources* pp_resources{nullptr};

    // Named resource registry (pass-to-pass data flow)
    ResourceRegistry* registry{nullptr};

    // Inverse view-projection for world-space reconstruction (DDGI sampling)
    glm::mat4 inv_view_proj{1.0f};

    // Skip shadow pass for this viewport (e.g. camera preview uses main viewport's shadows)
    bool skip_shadows{false};

    // Flip tonemap output vertically so render-to-texture viewports produce
    // images with V=0 at bottom (standard texture convention).
    bool flip_tonemap_y{false};

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

    // Pass registry for cross-pass record calls (e.g. sky/grid/debug from color_opaque)
    PassRegistry* passes{nullptr};

    // Game state for UI rendering
    bool playing{false};

    // UI rendering callbacks (set by game layer, used by transparent pass)
    using UiCallback = std::function<void(VkCommandBuffer, VkDescriptorSet,
                                          VkDescriptorSet, VkExtent2D, uint32_t)>;
    UiCallback world_ui_callback;
    UiCallback game_ui_callback;

    // Per-cascade shadow batches (indexed by cascade index)
    static constexpr uint32_t MAX_SHADOW_CASCADES = 3;
    std::array<IndirectBatch*, MAX_SHADOW_CASCADES> shadow_cascade_batches{};
    std::array<VkDescriptorSet, MAX_SHADOW_CASCADES> shadow_cascade_draw_data{};
    std::array<const std::vector<DrawCommand>*, MAX_SHADOW_CASCADES> shadow_cascade_commands{};
};

/// A named resource dependency with its intended usage.
struct ResourceUse {
    std::string name;
    ImageUsage usage;
};

/// Everything the pipeline needs to wire a pass: name, phase, ordering,
/// and resource reads/writes (used for both dependency sorting and
/// automatic FrameGraph barrier insertion).
struct PassSetup {
    std::string name;
    RenderPhase phase{RenderPhase::Color};
    uint32_t sub_order{0};
    std::vector<ResourceUse> reads;
    std::vector<ResourceUse> writes;
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

    /// Declare pass metadata: name, phase, resource reads/writes.
    /// The pipeline uses this for ordering, dependency resolution, and
    /// automatic FrameGraph barrier insertion.
    [[nodiscard]] virtual PassSetup setup() const { return {}; }

    /// Build this pass into the FrameGraph. Default implementation resolves
    /// setup() resource names to image IDs via the ResourceRegistry, then
    /// calls graph.add_pass() with record() as the execute callback.
    /// Complex passes override this with custom logic.
    virtual void build(FrameGraph& graph, ResourceRegistry& registry,
                       const FrameContext& ctx);

    /// Declare the pass's DAG topology: imported and created resources,
    /// reads, writes, queue assignment. The graph consumes this to
    /// schedule passes, allocate transient resources, and emit barriers.
    ///
    /// Default implementation does nothing. Passes implement this to opt
    /// into the DAG-authored path. Passes that don't are still driven by
    /// the legacy setup()/build() path. Both coexist during the
    /// migration; the legacy path will be removed once every pass has
    /// implemented declare().
    ///
    /// Must not record commands or touch per-frame transient state:
    /// called during graph construction, before any command buffer
    /// exists.
    virtual void declare(PassBuilder& /*builder*/, const DeclareContext& /*ctx*/) {}

    /// Whether this pass implements declare() and should be routed
    /// through the DAG path instead of the legacy build() path. Default
    /// is false; passes that implement declare() override to return true.
    /// Once every pass returns true, the legacy path is removed.
    [[nodiscard]] virtual bool uses_declare() const noexcept { return false; }

    [[nodiscard]] Result<> recreate(Device& device, const PassResources& resources) {
        destroy();
        return create(device, resources);
    }
};

} // namespace fjell
