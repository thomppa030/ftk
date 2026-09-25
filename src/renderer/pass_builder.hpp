#pragma once

#include "renderer/resource_desc.hpp"

#include <glm/vec2.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.h>

namespace fjell {

/// A named image made available to pass declare() bodies. The pipeline
/// registers target framebuffer images up front; producer passes add
/// their persistent outputs via RenderPass::collect_exports(). Passes
/// reach these by name through PassBuilder::import_named().
struct ImportedImage {
    VkImage image{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
    VkImageAspectFlags aspect{VK_IMAGE_ASPECT_COLOR_BIT};
    uint32_t base_layer{0};
    uint32_t layer_count{1};
    uint32_t mip_count{1};
    // Persistent images keep their tracked layout across begin_frame().
    // Set true for shadow atlases, Hi-Z pyramids, sky cubemaps — images
    // whose state at the start of frame N depends on frame N-1's exit
    // state.
    bool persistent{false};
    // The layout the owner keeps the image in outside the graph: where its
    // creation seeded it, or where every writer's closing barrier leaves it.
    // The graph starts from here when it has no memory of the image, so a
    // first read keeps what the owner put there instead of discarding it.
    // UNDEFINED for an image no pass reads before one writes it.
    VkImageLayout initial_layout{VK_IMAGE_LAYOUT_UNDEFINED};
};

/// A named buffer made available to pass declare() bodies, the buffer
/// counterpart of ImportedImage. Producer passes add their outputs through
/// RenderPass::collect_exports(); consumers reach them by name through
/// PassBuilder::import_named_buffer().
struct BufferImport {
    VkBuffer buffer{VK_NULL_HANDLE};
    VkDeviceSize size{0};
    // Read on a later frame than the one that wrote it, so a writer stays
    // alive with no reader in the frame.
    bool persistent{false};
};

/// A buffer a render subsystem's compute step writes and its draws then
/// read, with how the draws read it.
struct SubsystemDrawInput {
    BufferImport buffer;
    ResourceAccess draw_access{ResourceAccess::storage_buffer_read_vertex};
};

/// FNV-1a over a string_view. Used to key the DeclareContext::imports
/// catalog and PassBuilder transient names by a cheap 64-bit hash
/// instead of std::string — declare() is a hot per-frame path that
/// does hundreds of lookups, and the strings are all short and
/// well-known so collisions among the handful of names in play are
/// astronomically unlikely.
constexpr uint64_t fg_name_hash(std::string_view s) noexcept {
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char b : s) {
        h ^= static_cast<uint64_t>(b);
        h *= 1099511628211ULL;
    }
    return h;
}

/// State visible during pass declaration. Intentionally smaller than
/// FrameContext: only contains information that is legal to read when
/// the graph is being built (no command buffer, no transient pointers).
/// This is also the cache key for compiled-graph reuse in Phase 6.
struct DeclareContext {
    glm::uvec2 viewport_extent{0, 0};
    VkSampleCountFlagBits msaa_samples{VK_SAMPLE_COUNT_1_BIT};
    uint32_t frame_index{0};

    // Hardware capability flags
    bool mesh_shader_supported{false};
    bool ray_tracing_supported{false};

    // Scene summary — drives conditional topology ("only read shadow_atlas
    // if a directional light exists this frame"). Populated by the
    // pipeline before any pass's declare() runs.
    bool has_directional_light{false};
    bool has_volumetric_fog{false};
    bool has_world_environment{false};
    bool skip_shadows{false};

    /// Whether this run builds the bloom: bloom is on and this viewport's
    /// graph runs its chain. Off unless the render pipeline says so, so a
    /// reader never declares a bloom nothing writes.
    bool bloom_enabled{false};
    // Feature toggles resolved from engine settings
    bool ssr_enabled{true};
    bool gtao_enabled{true};
    bool taa_enabled{true};

    /// Whether the sea draws into this viewport this frame, and so whether
    /// "surface_depth" is an image of its own or another name for "depth".
    /// A pass that writes it declares that only when this is set.
    bool water_draws{false};
    bool auto_exposure_enabled{false};
    /// Whether this run builds the volumetric fog's froxel volume, so a pass
    /// may declare a read of its integrated result.
    bool volumetric_fog_enabled{false};
    bool ddgi_enabled{true};
    bool ss_gi_enabled{true};
    bool contact_shadows_enabled{true};

    /// Bitmask of ViewportSource enum values: which debug visualizations
    /// are currently showing somewhere (viewport tabs, overlays). Viz
    /// passes check their bit in declare() and flag side-effects only
    /// when they're actually going to produce a visible output.
    uint32_t active_sources{0};

    /// Named image catalog. Populated by the pipeline: target framebuffer
    /// images (color/depth/normal/…/screen_color) and any persistent
    /// images producer passes export via collect_exports(). Passes call
    /// PassBuilder::import_named(ctx, "name") to attach an access, which
    /// lets the graph see the cross-pass edge and emit the barrier
    /// automatically instead of passes hand-rolling inline transitions.
    ///
    /// Keyed by fg_name_hash(name) — declare() is a hot per-frame path
    /// that walked 150+ map lookups on std::string keys before; this
    /// drops string alloc + hashing entirely.
    std::unordered_map<uint64_t, ImportedImage> imports;

    /// Named buffer catalog, filled the same way as `imports` and keyed the
    /// same way. A buffer both a producer and its consumers declare is what
    /// gives the graph the edge between them and the barrier on it.
    std::unordered_map<uint64_t, BufferImport> buffer_imports;

    /// What the render subsystems' compute step (particle simulation)
    /// writes for their own draws. The subsystems are pluggable, so the
    /// passes that call their draw hooks cannot name these buffers; they
    /// declare reads on the whole list, and the subsystem compute pass
    /// declares the writes.
    std::vector<SubsystemDrawInput> subsystem_draw_inputs;
};

/// Records a pass's imported/created resources and its accesses against
/// them. Used by RenderPass::declare(). The graph consumes this to build
/// the DAG, assign queues, compute lifetimes, and emit barriers.
///
/// A pass declares:
///   - created resources: the graph allocates them (future: with aliasing)
///   - imported resources: externally-owned VkImage/VkBuffer handed to the graph
///   - reads / writes: accesses against handles
///
/// Writes and reads carry a ResourceAccess that drives both the DAG edge
/// type and the barrier the graph emits before the pass runs.
class PassBuilder {
public:
    // ── Resource creation & import ─────────────────────────────────────

    /// Declare a new transient resource. The graph allocates the VkImage
    /// and may alias it with other non-overlapping resources.
    FgTexture create(std::string_view name, const TextureDesc& desc);
    FgBuffer create(std::string_view name, const BufferDesc& desc);

    /// Declare an externally-owned resource. The graph tracks layout but
    /// does not allocate or destroy. Initial layout is what the pass
    /// expects the resource to be in at graph start; final layout is
    /// produced by the last access declared against the handle.
    FgTexture import(std::string_view name, VkImage image, VkImageView view,
                          VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                          uint32_t base_layer = 0, uint32_t layer_count = 1,
                          VkImageLayout initial_layout = VK_IMAGE_LAYOUT_UNDEFINED);
    /// `persistent=true` marks the buffer as cross-frame-live. Pass-cull
    /// keeps the producer alive even when no in-frame consumer reads it
    /// (the read happens next frame). Mirrors the image-side pattern used
    /// by exports like the shadow atlas.
    FgBuffer import(std::string_view name, VkBuffer buffer, VkDeviceSize size,
                    bool persistent = false);

    /// Import an image by the name it was registered under in the
    /// pipeline-provided DeclareContext catalog. Returns an invalid
    /// handle and warns if the name isn't present — typo-safe without
    /// crashing the frame.
    FgTexture import_named(const DeclareContext& ctx, std::string_view name);

    /// import_named() for an image some viewports do not have by design (a
    /// preview without GTAO or TAA): an invalid handle, and nothing logged,
    /// when the name is absent.
    FgTexture import_named_optional(const DeclareContext& ctx, std::string_view name);

    /// Import a buffer by the name a producer exported it under. Returns an
    /// invalid handle when no pass exported it this frame, which a consumer
    /// takes as "nothing to wait for": the producer is absent from this
    /// viewport's graph, or the buffer does not exist yet.
    FgBuffer import_named_buffer(const DeclareContext& ctx, std::string_view name);

    // ── Access declarations ────────────────────────────────────────────

    /// Declare that this pass reads a resource. Returns the handle so
    /// builder code can chain.
    FgTexture read(FgTexture, ResourceAccess);
    FgBuffer read(FgBuffer, ResourceAccess);

    /// Declare that this pass writes a resource.
    FgTexture write(FgTexture, ResourceAccess);
    FgBuffer write(FgBuffer, ResourceAccess);

    /// Declare that this pass reads and writes the same resource
    /// (storage image ping-pong, depth test+write, etc.).
    FgTexture read_write(FgTexture, ResourceAccess);
    FgBuffer read_write(FgBuffer, ResourceAccess);

    /// Promise that, after record() returns, the named texture is in
    /// the stated layout, for a pass that transitions it inline. The pass
    /// names its last write to the texture and the scope its own closing
    /// barrier made that write visible to; the graph records both without
    /// emitting a barrier, and puts one before any later reader outside
    /// that scope.
    FgTexture final_layout(FgTexture, VkImageLayout layout,
                           VkPipelineStageFlags2 written_stage, VkAccessFlags2 written_access,
                           VkPipelineStageFlags2 visible_stage, VkAccessFlags2 visible_access);

    // ── Pass-level flags ───────────────────────────────────────────────

    /// Assign this pass to a queue. Default is graphics.
    void queue(QueueType q) { queue_ = q; }

    /// Assign this pass to a parallel group. Passes in the same non-zero
    /// group can have their commands recorded in parallel onto secondary
    /// command buffers; group 0 (default) means sequential recording on
    /// the primary command buffer. The graph preserves declared
    /// dependencies regardless of group assignment.
    void parallel_group(uint32_t group) { parallel_group_ = group; }

    /// Mark this pass as never-cullable. Debug visualizations with
    /// side-effects that the graph can't see (ImGui, direct writes to
    /// host-visible buffers) use this.
    void never_cull() { never_cull_ = true; }

    /// The pass has side effects beyond its declared writes (e.g. writes
    /// to a persistent bindless descriptor, issues a host readback).
    /// Keeps it from being culled as "no-one reads my outputs".
    void has_side_effects() { side_effects_ = true; }

    // ── Introspection (used by FrameGraph during compile) ──────────────

    struct TextureAccess {
        FgTexture handle{};
        ResourceAccess access{ResourceAccess::sampled_fragment};
    };
    struct BufferAccess {
        FgBuffer handle{};
        ResourceAccess access{ResourceAccess::uniform_read};
    };
    struct ImportedTexture {
        FgTexture handle;
        std::string name;
        VkImage image;
        VkImageView view;
        VkImageAspectFlags aspect;
        uint32_t base_layer;
        uint32_t layer_count;
        uint32_t mip_count;
        VkImageLayout initial_layout;
        bool persistent;
    };
    struct ImportedBuffer {
        FgBuffer handle;
        std::string name;
        VkBuffer buffer;
        VkDeviceSize size;
        bool persistent;
    };
    struct CreatedTexture {
        FgTexture handle;
        std::string name;
        uint64_t name_hash;  // FNV-1a of name, computed in create()
        TextureDesc desc;
    };
    struct CreatedBuffer {
        FgBuffer handle;
        std::string name;
        uint64_t name_hash;  // FNV-1a of name, computed in create()
        BufferDesc desc;
    };
    struct FinalLayout {
        FgTexture handle{};
        VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
        VkPipelineStageFlags2 written_stage{0};
        VkAccessFlags2 written_access{0};
        VkPipelineStageFlags2 visible_stage{0};
        VkAccessFlags2 visible_access{0};
    };

    [[nodiscard]] const std::vector<TextureAccess>& texture_accesses() const noexcept { return texture_accesses_; }
    [[nodiscard]] const std::vector<BufferAccess>&  buffer_accesses()  const noexcept { return buffer_accesses_; }
    [[nodiscard]] const std::vector<ImportedTexture>& imported_textures() const noexcept { return imported_textures_; }
    [[nodiscard]] const std::vector<ImportedBuffer>&  imported_buffers()  const noexcept { return imported_buffers_; }
    [[nodiscard]] const std::vector<CreatedTexture>& created_textures() const noexcept { return created_textures_; }
    [[nodiscard]] const std::vector<CreatedBuffer>&  created_buffers()  const noexcept { return created_buffers_; }
    [[nodiscard]] const std::vector<FinalLayout>& final_layouts() const noexcept { return final_layouts_; }
    [[nodiscard]] QueueType queue() const noexcept { return queue_; }
    [[nodiscard]] uint32_t parallel_group() const noexcept { return parallel_group_; }
    [[nodiscard]] bool is_never_cull() const noexcept { return never_cull_; }
    [[nodiscard]] bool has_side_effects_flag() const noexcept { return side_effects_; }

    /// Reset for a new pass. Used by the graph when it hands the builder
    /// to the next pass's declare().
    void reset();

private:
    std::vector<TextureAccess> texture_accesses_;
    std::vector<BufferAccess>  buffer_accesses_;
    std::vector<ImportedTexture> imported_textures_;
    std::vector<ImportedBuffer>  imported_buffers_;
    std::vector<CreatedTexture>  created_textures_;
    std::vector<CreatedBuffer>   created_buffers_;
    std::vector<FinalLayout>     final_layouts_;

    QueueType queue_{QueueType::graphics};
    uint32_t parallel_group_{0};
    bool never_cull_{false};
    bool side_effects_{false};

    // Resource IDs are assigned by an owning FrameGraph; the builder
    // doesn't know its owner, so it uses a private monotonic counter
    // and the graph maps builder-local IDs into its own space on submit.
    uint32_t next_texture_id_{0};
    uint32_t next_buffer_id_{0};
};

} // namespace fjell
