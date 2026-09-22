#pragma once

#include "renderer/resource_desc.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace fjell {

class PassBuilder;
class ThreadPool;
class ThreadCommandPools;

// How a pass uses an image
enum class ImageUsage : uint8_t {
    color_attachment,       // write as color render target
    depth_attachment,       // write as depth render target
    depth_attachment_read,  // read depth (no write) during rendering
    shader_read,            // sample in a fragment shader
    compute_read,           // sample in a compute shader
    compute_storage_read,   // read as a storage image in a compute shader (GENERAL)
    compute_write,          // write as a storage image in a compute shader
    raytracing_read,        // sample in a ray tracing shader
    raytracing_write,       // write as a storage image in a ray tracing shader
    transfer_src,           // source of a copy/blit operation
    transfer_dst,           // destination of a copy/blit operation
};

// Subresource range tracked by the frame graph. A subrange of an image
// with its own layout / last access state.
struct SubresourceRange {
    VkImageAspectFlags aspect{VK_IMAGE_ASPECT_COLOR_BIT};
    uint32_t base_mip{0};
    uint32_t mip_count{1};
    uint32_t base_layer{0};
    uint32_t layer_count{1};
};

// State for one non-overlapping slice of a tracked image.
struct ImageSlice {
    SubresourceRange range;
    VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
    VkPipelineStageFlags2 last_stage{VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT};
    VkAccessFlags2 last_access{0};
};

// Image tracked by the frame graph. Layout/access state is per-slice so
// that passes touching specific mips or array layers don't invalidate
// the rest of the image.
struct TrackedImage {
    VkImage image{VK_NULL_HANDLE};
    VkImageAspectFlags aspect{VK_IMAGE_ASPECT_COLOR_BIT};
    uint32_t mip_count{1};
    uint32_t array_layers{1};
    uint32_t base_layer{0};
    bool persistent{false};   // state carries across begin_frame()

    // Transient resources declared through PassBuilder::create(). For
    // C2 (lifetime analysis + bin-packing bookkeeping) these hold only
    // the TextureDesc for matching; C3.1 added VMA-backed allocation,
    // C3.2 registers the view under `name` into ResourceRegistry so
    // passes can read it through ctx.registry->image_view(name).
    bool virtual_resource{false};
    TextureDesc desc{};
    std::string name;

    std::vector<ImageSlice> slices;
};

// One (image, subresource, usage) tuple inside a pass declaration.
struct ImageAccess {
    uint32_t image_id{0};
    ImageUsage usage{ImageUsage::shader_read};
    SubresourceRange range{};
};

// A post-pass state override. The pass promises that after its
// record() returns, the named subresource is in the stated layout —
// the graph records it without emitting a barrier.
struct FinalLayoutOverride {
    uint32_t image_id{0};
    SubresourceRange range{};
    VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
    VkPipelineStageFlags2 last_stage{VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    VkAccessFlags2 last_access{0};
};

// A pass declaration: what images it reads and writes
struct PassDecl {
    std::string name;
    std::function<void(VkCommandBuffer)> execute;
    std::vector<ImageAccess> image_uses;
    std::vector<FinalLayoutOverride> final_layouts;
    uint32_t parallel_group{0}; // 0 = sequential, >0 = parallel group ID
    // Which queue should record this pass. Phase 3b reads this to split
    // the DAG across graphics and async compute command buffers; Phase 3a
    // only classifies so the segment-boundary picture can be logged and
    // validated before we actually split.
    QueueType queue{QueueType::graphics};
};

// Lifetime of a tracked image over the current pass list, expressed as
// half-open [first_pass, last_pass] pass indices into passes_. Unused
// resources leave the sentinel values in place.
struct ResourceLifetime {
    uint32_t first_pass{UINT32_MAX};
    uint32_t last_pass{0};
    VkImageUsageFlags usage_flags{0};

    [[nodiscard]] bool used() const noexcept { return first_pass != UINT32_MAX; }
};

// One physical allocation shared by resources whose lifetimes don't
// overlap. Populated by compute_alias_groups() from lifetime + desc.
// Non-matching descs (different format/extent/samples/layers/mips) go
// into their own groups — exact match only in Phase 3.
struct AliasGroup {
    TextureDesc desc{};                   // shared descriptor, for C3 allocation
    std::vector<uint32_t> resource_ids;   // image IDs in images_ that share this group
    uint32_t last_free_pass{0};           // highest last_pass among members; used during packing
};

// Lightweight frame graph that tracks image layouts and inserts barriers.
// Not a full dependency graph — pass order is explicit, the graph just
// handles transitions.
class FrameGraph {
public:
    // Register an image to track. Returns an ID.
    uint32_t register_image(VkImage image, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                            uint32_t base_layer = 0, uint32_t layer_count = 1,
                            uint32_t mip_count = 1,
                            bool persistent = false);

    // Start a new frame — reset all layouts to UNDEFINED
    void begin_frame();

    // Add a pass that uses images. The execute callback records commands into
    // the provided command buffer (primary for sequential, secondary for parallel).
    void add_pass(const std::string& name, std::function<void(VkCommandBuffer)> execute,
                  std::initializer_list<std::pair<uint32_t, ImageUsage>> uses,
                  uint32_t parallel_group = 0);
    void add_pass(const std::string& name, std::function<void(VkCommandBuffer)> execute,
                  std::vector<std::pair<uint32_t, ImageUsage>> uses,
                  uint32_t parallel_group = 0);

    /// Submit a DAG-authored pass: consumes a PassBuilder (populated by
    /// RenderPass::declare() or a pass's build()) plus the record
    /// callback. Registers all imported images into the graph, derives
    /// legacy ImageUsage values from declared ResourceAccess, reads
    /// parallel-group assignment from the builder, and enqueues the
    /// pass the same way add_pass() does. Created (non-imported)
    /// resources are not yet allocated — that lands with Phase 3
    /// aliasing. Passing a builder with created resources is an error
    /// until then.
    void submit_declared_pass(const std::string& name, const PassBuilder& builder,
                              std::function<void(VkCommandBuffer)> execute);

    // Execute all passes, inserting barriers between them.
    // Passes with the same parallel_group > 0 are recorded in parallel on
    // secondary command buffers via the thread pool.
    //
    // Three command buffers, split around the async compute island:
    //   graphics_pre  — graphics passes before the first compute pass.
    //   async_compute — compute-hinted passes (null routes back to pre).
    //   graphics_post — graphics passes after the last compute pass.
    //                   null folds them back into pre, which is the
    //                   correct behavior when no compute pass runs.
    //
    // Returns true if any pass was actually recorded into the async
    // compute CB — submit_and_present uses this to fire the compute
    // submit + timeline sync only when needed.
    [[nodiscard]] bool execute(VkCommandBuffer graphics_pre,
                               VkCommandBuffer graphics_post,
                               VkCommandBuffer async_compute,
                               ThreadPool* pool, ThreadCommandPools* cmd_pools,
                               uint32_t frame_index);

    // Compute per-image lifetime (first/last pass index, unioned image
    // usage flags) over the currently submitted pass list. Indices point
    // into passes_ in submission order — the pipeline submits in DAG
    // order, so these double as DAG-order lifetimes.
    [[nodiscard]] std::vector<ResourceLifetime> compute_lifetimes() const;

    // Log per-image lifetimes through the graphics logger. Gated by the
    // FJELL_LOG_LIFETIMES env var so enabling it on demand is a no-rebuild
    // operation. Intended for Phase 3 debugging only.
    void log_lifetimes() const;

    // Greedy bin-pack virtual (create()-declared) resources with
    // non-overlapping lifetimes and identical descriptors into shared
    // AliasGroups. Persistent and backed (imported) images stay out of
    // the pool — each lands in its own singleton group. C3 turns these
    // groups into real VkImage allocations; C2 only analyses.
    //
    // When aliasing is disabled via set_aliasing_enabled(false), every
    // candidate is returned as its own singleton group — the pool then
    // allocates one distinct VkImage per logical resource. Used as a
    // debug kill switch for A/B regression hunts.
    [[nodiscard]] std::vector<AliasGroup> compute_alias_groups() const;

    // Toggle whether compute_alias_groups() performs greedy packing.
    // Default is on; flipping off yields one physical image per logical
    // (no aliasing). Intended for debug UI only.
    void set_aliasing_enabled(bool enabled) noexcept { aliasing_enabled_ = enabled; }
    [[nodiscard]] bool aliasing_enabled() const noexcept { return aliasing_enabled_; }

    // Log the alias groups and the logical-to-physical ratio. Same
    // FJELL_LOG_LIFETIMES gate and same one-shot cadence as
    // log_lifetimes().
    void log_alias_groups() const;

    // Walk every alias group and assert that its members have disjoint
    // lifetimes (last pass of member i strictly before first pass of
    // member i+1). Returns true if everything is legal. Gated by
    // FJELL_VALIDATE_ALIASING so it only runs on demand.
    [[nodiscard]] bool validate_alias_groups(const std::vector<AliasGroup>& groups) const;

    // Log queue-segment breakdown — how the submitted pass list splits
    // into runs of same-queue passes. Each segment boundary is a future
    // timeline-semaphore sync point. Same FJELL_LOG_LIFETIMES gate and
    // cadence as log_alias_groups().
    void log_queue_segments() const;

    // Accessor for the submitted image list. The pipeline reads this
    // after compute_alias_groups() to wire group allocations back to
    // the named logical resources in ResourceRegistry.
    [[nodiscard]] const std::vector<TrackedImage>& images() const noexcept { return images_; }

    // Bind a VkImage to a virtual (create()-declared) resource after the
    // TransientImagePool assigns a physical allocation. Must be called
    // before execute() so barrier emission can operate on the real
    // image. Slice state stays at the begin_frame() seed (UNDEFINED) so
    // the first access triggers a correct aliasing transition whether
    // the pool handed back a fresh image or one reused from last frame.
    void bind_virtual_image(uint32_t image_id, VkImage image);

private:
    static VkImageLayout layout_for(ImageUsage usage, VkImageAspectFlags aspect);
    static VkPipelineStageFlags2 stage_for(ImageUsage usage);
    static VkAccessFlags2 access_for(ImageUsage usage);

    // When a barrier is recorded into a compute-queue CB, graphics-only
    // stage bits (COLOR_ATTACHMENT_OUTPUT, FRAGMENT_SHADER, vertex/tess/
    // geometry, EARLY/LATE_FRAGMENT_TESTS) are rejected by the validator
    // because they're not part of VK_QUEUE_COMPUTE_BIT's supported set.
    // Cross-queue synchronisation between graphics_pre's signal and the
    // compute wait is already handled by the timeline semaphore in
    // submit_and_present, so rewriting srcStage on the compute CB to
    // ALL_COMMANDS is safe: we aren't dropping any ordering, just naming
    // a stage the queue actually supports. Same logic applies to any
    // graphics-only dstStage coming back the other direction.
    static VkPipelineStageFlags2 stages_for_queue(VkPipelineStageFlags2 stages,
                                                  QueueType queue);

    // Split the slice list so that every slice is either fully inside
    // the query range or fully outside it. Returns indices into img.slices
    // for the slices that cover the range.
    std::vector<size_t> carve_slices(TrackedImage& img, const SubresourceRange& range);

    // Merge neighbouring slices whose state is identical. Called after
    // updating state so the list doesn't grow unboundedly.
    void coalesce_slices(TrackedImage& img);

    void insert_barrier_for_slice(VkCommandBuffer cmd, const TrackedImage& img,
                                   ImageSlice& slice,
                                   VkImageLayout new_layout,
                                   VkPipelineStageFlags2 dst_stage,
                                   VkAccessFlags2 dst_access,
                                   QueueType queue);

    void emit_barriers_for_pass(VkCommandBuffer cmd, const PassDecl& pass);

    // Apply a pass's final_layout overrides: patch slice state to the
    // declared values without emitting any barrier (producer promises
    // the image already ends up in that layout).
    void apply_final_layouts(const PassDecl& pass);

    std::vector<TrackedImage> images_;
    std::vector<PassDecl> passes_;

    // Reusable scratch for parallel group secondary command buffers
    std::vector<VkCommandBuffer> secondaries_scratch_;

    bool aliasing_enabled_{true};
};

} // namespace fjell
