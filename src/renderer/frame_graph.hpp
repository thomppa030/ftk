#pragma once

#include "renderer/resource_desc.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
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
    compute_read_write,     // load and store a storage image in a compute shader
    depth_read_sampled,     // depth-test against an image the fragment shader also samples
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
    uint64_t name_hash{0};

    std::vector<ImageSlice> slices;
};

// Identity of an imported image inside the graph: the same VkImage seen
// through different layer ranges is tracked apart (shadow cascades).
struct ImageKey {
    VkImage image{VK_NULL_HANDLE};
    uint32_t base_layer{0};
    uint32_t layer_count{1};
    bool operator==(const ImageKey&) const noexcept = default;
};

struct ImageKeyHash {
    size_t operator()(const ImageKey& k) const noexcept {
        auto h = std::hash<void*>{}(static_cast<void*>(k.image));
        h ^= std::hash<uint32_t>{}(k.base_layer) << 1;
        h ^= std::hash<uint32_t>{}(k.layer_count) << 2;
        return h;
    }
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

// Tracks image layouts across the passes of one pipeline run and inserts
// the barriers between them. Not a dependency graph — pass order is decided
// by RenderPipeline, the graph handles transitions.
//
// One instance serves every run: begin_frame() empties it for the next
// call, and what it learned about each imported image's layout is kept
// across calls, so a run starts from the layout the previous one left an
// image in rather than from UNDEFINED. That is what lets a history image
// read first in a frame keep its contents, and what gives the first barrier
// of a frame a real source scope against the previous frame's readers.
class FrameGraph {
public:
    // Register an image to track and return its id. Registering the same
    // image and layer range again in one run returns the existing id. A new
    // entry starts from what the graph remembers of the image, else from
    // `initial_layout`.
    uint32_t register_image(VkImage image, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                            uint32_t base_layer = 0, uint32_t layer_count = 1,
                            uint32_t mip_count = 1,
                            bool persistent = false,
                            VkImageLayout initial_layout = VK_IMAGE_LAYOUT_UNDEFINED);

    // Once per frame, before any run: forget the state of images no run
    // touched last frame, so a destroyed image's handle cannot come back
    // with a stale layout attached.
    void new_frame();

    // Once per run: drop the previous run's passes and images. The
    // remembered layouts survive.
    void begin_frame();

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
    void compute_lifetimes(std::vector<ResourceLifetime>& out) const;
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
    void compute_alias_groups(const std::vector<ResourceLifetime>& lifetimes,
                              std::vector<AliasGroup>& out) const;
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
    // the query range or fully outside it. Fills `out` with indices into
    // img.slices for the slices that cover the range.
    void carve_slices(TrackedImage& img, const SubresourceRange& range,
                      std::vector<size_t>& out);

    // Merge neighbouring slices whose state is identical. Called after
    // updating state so the list doesn't grow unboundedly.
    void coalesce_slices(TrackedImage& img);

    // Append the barrier that brings one slice to the requested state to
    // barriers_scratch_, and record the new state on the slice. Nothing is
    // appended when the slice is already visible to that access.
    void append_barrier_for_slice(VkCommandBuffer cmd, const TrackedImage& img,
                                  ImageSlice& slice,
                                  VkImageLayout new_layout,
                                  VkPipelineStageFlags2 dst_stage,
                                  VkAccessFlags2 dst_access,
                                  QueueType queue);

    // Every barrier a pass needs, issued as one vkCmdPipelineBarrier2.
    void emit_barriers_for_pass(VkCommandBuffer cmd, const PassDecl& pass);

    // After a run: store every imported image's slice state for the next
    // run to start from.
    void remember_states();

    // One image's merged pre-pass state, accumulated across all of the
    // pass's declared accesses: one barrier per image per pass, combining
    // every stage and access and a layout compatible with all of them.
    struct MergedAccess {
        uint32_t image_id{0};
        SubresourceRange range{};
        VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
        VkPipelineStageFlags2 stages{0};
        VkAccessFlags2 access{0};
        bool any_write{false};
    };

    // What a run left an imported image in, keyed by the image; `seen` is
    // the frame that last stored it.
    struct RememberedState {
        std::vector<ImageSlice> slices;
        uint64_t seen{0};
    };

    // Apply a pass's final_layout overrides: patch slice state to the
    // declared values without emitting any barrier (producer promises
    // the image already ends up in that layout).
    void apply_final_layouts(const PassDecl& pass);

    std::vector<TrackedImage> images_;
    std::vector<PassDecl> passes_;

    // Per-run lookup from image identity, and from transient name, to an
    // index into images_.
    std::unordered_map<ImageKey, uint32_t, ImageKeyHash> image_index_;
    std::unordered_map<uint64_t, uint32_t> virtual_index_;

    std::unordered_map<ImageKey, RememberedState, ImageKeyHash> remembered_;
    uint64_t frame_serial_{0};

    // Reusable scratch, so a run allocates nothing on its hot path.
    std::vector<VkCommandBuffer> secondaries_scratch_;
    std::vector<VkImageMemoryBarrier2> barriers_scratch_;
    std::vector<ImageSlice> rebuilt_scratch_;
    std::vector<size_t> indices_scratch_;
    std::vector<MergedAccess> merged_scratch_;

    bool aliasing_enabled_{true};
};

} // namespace fjell
