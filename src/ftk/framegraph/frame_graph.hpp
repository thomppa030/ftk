#pragma once

#include "ftk/base/handle.hpp"
#include "ftk/framegraph/resource_desc.hpp"
#include "ftk/gpu/access.hpp"
#include "ftk/gpu/buffer.hpp"
#include "ftk/gpu/queue.hpp"
#include "ftk/gpu/texture.hpp"
#include "ftk/gpu/transition.hpp"


#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fjell {

namespace gpu {
class CommandList;
class Device;
class Frame;
}

class PassBuilder;
class ThreadPool;

// Subresource range tracked by the frame graph: a part of an image with a
// state of its own. A count of gpu::TextureView::REST reaches to the last.
struct SubresourceRange {
    uint32_t base_mip{0};
    uint32_t mip_count{1};
    uint32_t base_layer{0};
    uint32_t layer_count{1};
};

// Synchronisation state of one tracked resource, or one slice of an image,
// in accesses: what last wrote it, what has read it since, and what that
// write has already been made visible to. The backend turns accesses into
// stages and access masks, for the queue a barrier is recorded on, only when
// it builds one.
struct AccessState {
    gpu::AccessSet written_by{};   // empty: no write seen
    // Reads whose barrier also changed the image's layout: later work waits
    // for them as for the write, since the change is ordered ahead of them.
    gpu::AccessSet changed_for{};
    gpu::AccessSet read_by{};      // every reader since that write
    // What the last barrier made the write visible to, widened by each read
    // barrier since, so a reader it covers needs no barrier of its own.
    gpu::AccessSet visible_to{};

    bool operator==(const AccessState&) const noexcept = default;
};

// State for one non-overlapping slice of a tracked image.
struct ImageSlice {
    SubresourceRange range;
    // The accesses it is in, which give its layout; empty for nothing it
    // holds being kept (undefined).
    gpu::AccessSet in{};
    AccessState state;
};

// Image tracked by the frame graph. Layout/access state is per-slice so
// that passes touching specific mips or array layers don't invalidate
// the rest of the image.
struct TrackedImage {
    /// Invalid for a transient until the pool backs it.
    gpu::Texture texture{};
    /// Whether it holds depth, which decides the states some accesses need.
    bool depth{false};
    uint32_t mip_count{1};
    uint32_t array_layers{1};
    uint32_t base_layer{0};
    bool persistent{false};   // state carries across begin_frame()
    /// Where the image rests between the passes that declare it; empty when
    /// only the graph's passes touch it.
    gpu::AccessSet resting{};

    // Transient resources declared through PassBuilder::create(). For
    // C2 (lifetime analysis + bin-packing bookkeeping) these hold only
    // the TextureDesc for matching; C3.1 added VMA-backed allocation,
    // C3.2 registers the texture under `name` into ResourceRegistry so
    // passes can read it through ctx.texture(name).
    bool virtual_resource{false};
    TextureDesc desc{};
    std::string name;
    uint64_t name_hash{0};

    std::vector<ImageSlice> slices;
};

// Identity of an imported image inside the graph: the same texture seen
// through different layer ranges is tracked apart.
struct ImageKey {
    gpu::Texture texture{};
    uint32_t base_layer{0};
    uint32_t layer_count{1};
    bool operator==(const ImageKey&) const noexcept = default;
};

struct ImageKeyHash {
    size_t operator()(const ImageKey& k) const noexcept {
        auto h = std::hash<uint32_t>{}(k.texture.id);
        h ^= std::hash<uint32_t>{}(k.base_layer) << 1;
        h ^= std::hash<uint32_t>{}(k.layer_count) << 2;
        return h;
    }
};

// Buffer tracked by the frame graph, as a whole: no pass declares part of
// one. An acceleration structure is tracked the same way, `structure` set
// and `buffer` not.
struct TrackedBuffer {
    gpu::Buffer buffer{};
    gpu::AccelerationStructure structure{};
    bool persistent{false};
    AccessState state;
    /// The name it was declared under; kept only while the barrier trace
    /// records.
    std::string name;
};

// One pass's use of a buffer.
struct BufferUse {
    uint32_t buffer_id{0};
    gpu::AccessSet access{};
    bool read{false};
    bool write{false};
};

// One (image, subresource, access) tuple inside a pass declaration.
struct ImageAccess {
    uint32_t image_id{0};
    gpu::Access access{gpu::Access::sampled_fragment};
    SubresourceRange range{};
};

// What a pass leaves a texture in when its own work moved it (a mip chain):
// its last writes were `written_by`, and it is left as `left_as`, those
// writes visible there. The graph takes it as the state after the pass
// without a barrier, and later uses outside `left_as` get one.
struct FinalState {
    uint32_t image_id{0};
    SubresourceRange range{};
    gpu::AccessSet written_by{};
    gpu::AccessSet left_as{};
};

// A pass declaration: what images it reads and writes
struct PassDecl {
    std::string name;
    /// Records the pass into the list it is given: none in a run without a
    /// device, as the unit tests make.
    std::function<void(gpu::CommandList*)> execute;
    std::vector<ImageAccess> image_uses;
    std::vector<BufferUse> buffer_uses;
    std::vector<FinalState> final_states;
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
    gpu::TextureUses uses{};

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

// What the graph needs to know of a texture it is handed.
struct TextureShape {
    uint32_t mips{1};
    uint32_t layers{1};
    bool depth{false};
};

// What the graph needs from outside itself for a run: the shape of a texture
// it is handed, where the transitions it works out are recorded, and how a
// state reads in its traces. The engine's comes from the GPU device; a test
// makes its own, which is what lets it run the graph without a GPU.
struct GraphHost {
    std::function<TextureShape(gpu::Texture)> shape;
    /// Records `transitions` into `list` on `queue`, and describes each
    /// barrier they became as a line in `trace` when that is not null. The
    /// list is none in a run without a device.
    std::function<void(gpu::CommandList* list, gpu::Queue queue,
                       std::span<const gpu::Transition> transitions, std::string* trace)>
        record;
    /// A state as the traces name it (on Vulkan, its layout).
    std::function<std::string(gpu::AccessSet state, bool depth)> state_name;
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
    // Register a texture to track, every mip and the layers `view` covers,
    // and return its id. Registering the same texture and layers again in
    // one run returns the existing id. A new entry starts from what the
    // graph remembers of it, else at rest when it has a resting access and
    // has been written, else undefined. `name` is only for the traces.
    uint32_t register_image(const gpu::TextureView& view, bool persistent = false,
                            std::string_view name = {}, gpu::AccessSet resting = {},
                            bool unwritten = false);

    // Register a buffer to track and return its id; the same buffer again
    // in one run returns the existing id. A new entry starts from what the
    // graph remembers of the buffer, else with no write to wait for.
    uint32_t register_buffer(gpu::Buffer buffer, bool persistent = false,
                             std::string_view name = {});
    // register_buffer() for an acceleration structure, among the buffers.
    uint32_t register_acceleration(gpu::AccelerationStructure structure, bool persistent = false,
                                   std::string_view name = {});

    // Once per frame, before any run: forget the state of images and
    // buffers no run touched last frame, so a destroyed handle cannot come
    // back with stale state attached.
    void new_frame();

    // Once per run: drop the previous run's passes and images. The
    // remembered layouts survive. The run's textures and buffers are the
    // device's, and its transitions are recorded through it.
    void begin_frame(gpu::Device& device);

    // begin_frame() with `host` in the device's place, which must outlive
    // the run.
    void begin_frame(const GraphHost& host);

    /// Submit a DAG-authored pass: consumes a PassBuilder (populated by
    /// RenderPass::declare() or a pass's build()) plus the record
    /// callback. Registers all imported images into the graph, keeps the
    /// declared gpu::Access of each, reads
    /// parallel-group assignment from the builder, and enqueues the
    /// pass the same way add_pass() does. Created (non-imported)
    /// resources are not yet allocated — that lands with Phase 3
    /// aliasing. Passing a builder with created resources is an error
    /// until then.
    void submit_declared_pass(const std::string& name, const PassBuilder& builder,
                              std::function<void(gpu::CommandList*)> execute);

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
    [[nodiscard]] bool execute(gpu::CommandList* graphics_pre,
                               gpu::CommandList* graphics_post,
                               gpu::CommandList* async_compute,
                               ThreadPool* pool, gpu::Frame* frame);

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
    // groups into real texture allocations; C2 only analyses.
    //
    // When aliasing is disabled via set_aliasing_enabled(false), every
    // candidate is returned as its own singleton group — the pool then
    // allocates one distinct texture per logical resource. Used as a
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

    // Bind a texture to a virtual (create()-declared) resource after the
    // TransientImagePool assigns one. Must be called before execute() so
    // barrier emission can operate on the real image. Slice state stays at
    // the begin_frame() seed (UNDEFINED) so the first access triggers a
    // correct aliasing transition whether the pool handed back a fresh
    // texture or one reused from last frame.
    void bind_virtual_image(uint32_t image_id, gpu::Texture texture);

private:
    // The GPU interface's name for the queue a pass records on.
    [[nodiscard]] static gpu::Queue gpu_queue(QueueType queue);

    // The host a run on `device` works with: shapes from what the device
    // made or was handed, transitions recorded through its command lists.
    [[nodiscard]] static GraphHost device_host(gpu::Device& device);

    // `range` of `img` as a view of its texture.
    [[nodiscard]] static gpu::TextureView image_range(const TrackedImage& img,
                                                      const SubresourceRange& range);

    // Split the slice list so that every slice is either fully inside
    // the query range or fully outside it. Fills `out` with indices into
    // img.slices for the slices that cover the range.
    void carve_slices(TrackedImage& img, const SubresourceRange& range,
                      std::vector<size_t>& out);

    // Merge neighbouring slices whose state is identical. Called after
    // updating state so the list doesn't grow unboundedly.
    void coalesce_slices(TrackedImage& img);

    // Append the transition that brings one slice to `accesses` to
    // transitions_scratch_, and record the new state on the slice. Nothing
    // is appended when the slice is already visible to those accesses.
    void append_barrier_for_slice(const TrackedImage& img, ImageSlice& slice,
                                  gpu::AccessSet accesses, bool write, QueueType queue);

    // Append the transition one pass's use of a buffer needs to
    // transitions_scratch_, and record the use on the buffer's state.
    // Nothing is appended when the use has nothing to wait for.
    void append_barrier_for_buffer(TrackedBuffer& buf, const BufferUse& use,
                                   QueueType queue);

    // Every transition a pass needs, image and buffer, recorded as one
    // batch.
    void emit_barriers_for_pass(gpu::CommandList* list, const PassDecl& pass);

    // Records transitions_scratch_ into `cmd` on `queue`, describing each
    // barrier in trace_pending_ while the barrier trace records.
    void record_transitions(gpu::CommandList* list, QueueType queue);

    // After a run: store every imported image's slice state for the next
    // run to start from.
    void remember_states();

    // One image's merged pre-pass state, accumulated across all of the
    // pass's declared accesses: one barrier per image per pass, for every
    // access in one layout that serves them all.
    struct MergedAccess {
        uint32_t image_id{0};
        SubresourceRange range{};
        gpu::AccessSet accesses{};
        bool any_write{false};
    };

    // What a run left an imported image in, keyed by the image; `seen` is
    // the frame that last stored it.
    struct RememberedState {
        std::vector<ImageSlice> slices;
        uint64_t seen{0};
    };
    struct RememberedBuffer {
        AccessState state;
        uint64_t seen{0};
    };

    // Apply what a pass says it leaves its textures in: patch slice state
    // to it without emitting any barrier (the pass's own work got it there).
    void apply_final_states(const PassDecl& pass);

    // After a pass, in its command buffer: return each resting image it
    // declared to rest, as a read by the resting access would, so nothing
    // is emitted for one already there.
    void return_to_rest(gpu::CommandList* list, const PassDecl& pass);

    // The run's host, and the one made for the device begin_frame() was
    // last handed.
    const GraphHost* host_{nullptr};
    GraphHost device_host_;
    gpu::Device* device_host_for_{nullptr};
    std::vector<TrackedImage> images_;
    std::vector<TrackedBuffer> buffers_;
    std::vector<PassDecl> passes_;

    // Per-run lookup from image identity, and from transient name, to an
    // index into images_.
    std::unordered_map<ImageKey, uint32_t, ImageKeyHash> image_index_;
    std::unordered_map<uint64_t, uint32_t> virtual_index_;

    std::unordered_map<gpu::Buffer, uint32_t, HandleHash> buffer_index_;
    std::unordered_map<gpu::AccelerationStructure, uint32_t, HandleHash> acceleration_index_;

    /// Whether this run is in the barrier trace's window
    /// (FJELL_LOG_BARRIERS), and which run of the frame it is.
    bool tracing_{false};
    uint32_t run_in_frame_{0};
    /// Barrier lines for the trace, written out after the line that says
    /// what they are for.
    std::string trace_pending_;

    std::unordered_map<ImageKey, RememberedState, ImageKeyHash> remembered_;
    std::unordered_map<gpu::Buffer, RememberedBuffer, HandleHash> remembered_buffers_;
    std::unordered_map<gpu::AccelerationStructure, RememberedBuffer, HandleHash> remembered_accelerations_;
    uint64_t frame_serial_{0};

    // Reusable scratch, so a run allocates nothing on its hot path.
    std::vector<gpu::CommandList*> parallel_scratch_;
    std::vector<gpu::Transition> transitions_scratch_;
    std::vector<BufferUse> merged_buffers_scratch_;
    std::vector<uint32_t> handle_to_buffer_scratch_;
    std::vector<ImageSlice> rebuilt_scratch_;
    std::vector<size_t> indices_scratch_;
    std::vector<MergedAccess> merged_scratch_;

    bool aliasing_enabled_{true};
};

} // namespace fjell
