#pragma once

#include "renderer/resource_desc.hpp"

#include <glm/vec2.hpp>
#include <string>
#include <string_view>
#include <vector>
#include <vulkan/vulkan.h>

namespace fjell {

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

    // Feature toggles resolved from engine settings
    bool bloom_enabled{true};
    bool ssr_enabled{true};
    bool gtao_enabled{true};
    bool taa_enabled{true};
    bool auto_exposure_enabled{false};
    bool volumetric_fog_enabled{true};
    bool ddgi_enabled{true};
    bool ss_gi_enabled{true};
    bool contact_shadows_enabled{true};
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
    FgBuffer import(std::string_view name, VkBuffer buffer, VkDeviceSize size);

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

    // ── Pass-level flags ───────────────────────────────────────────────

    /// Assign this pass to a queue. Default is graphics.
    void queue(QueueType q) { queue_ = q; }

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
        VkImageLayout initial_layout;
    };
    struct ImportedBuffer {
        FgBuffer handle;
        std::string name;
        VkBuffer buffer;
        VkDeviceSize size;
    };
    struct CreatedTexture {
        FgTexture handle;
        std::string name;
        TextureDesc desc;
    };
    struct CreatedBuffer {
        FgBuffer handle;
        std::string name;
        BufferDesc desc;
    };

    [[nodiscard]] const std::vector<TextureAccess>& texture_accesses() const noexcept { return texture_accesses_; }
    [[nodiscard]] const std::vector<BufferAccess>&  buffer_accesses()  const noexcept { return buffer_accesses_; }
    [[nodiscard]] const std::vector<ImportedTexture>& imported_textures() const noexcept { return imported_textures_; }
    [[nodiscard]] const std::vector<ImportedBuffer>&  imported_buffers()  const noexcept { return imported_buffers_; }
    [[nodiscard]] const std::vector<CreatedTexture>& created_textures() const noexcept { return created_textures_; }
    [[nodiscard]] const std::vector<CreatedBuffer>&  created_buffers()  const noexcept { return created_buffers_; }
    [[nodiscard]] QueueType queue() const noexcept { return queue_; }
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

    QueueType queue_{QueueType::graphics};
    bool never_cull_{false};
    bool side_effects_{false};

    // Resource IDs are assigned by an owning FrameGraph; the builder
    // doesn't know its owner, so it uses a private monotonic counter
    // and the graph maps builder-local IDs into its own space on submit.
    uint32_t next_texture_id_{0};
    uint32_t next_buffer_id_{0};
};

} // namespace fjell
