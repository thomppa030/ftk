#pragma once

#include "renderer/pass/render_pass.hpp"

#include <glm/glm.hpp>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/named_value.hpp"
#include "renderer/renderer_constants.hpp"

namespace fjell {

class BindlessTextures;
class PipelineCache;

// ── Froxel-based volumetric fog pipeline ────────────────────────────────────
//
// Fog runs four compute dispatches authored as four independent DAG nodes:
//
//   fog_inject     — write density + light scattering into scatter[cur].
//                    The shader is the fog node's material, a `shader_type
//                    fog` .fjsl compiled through PipelineCache into the fog
//                    template; the engine's fog.fjsl when the node has none.
//   fog_temporal   — blend scatter[cur] with scatter[prev] (the last
//                    frame's result). Reads scatter[prev], reads+writes
//                    scatter[cur].
//   fog_integrate  — front-to-back ray march scatter[cur] into the
//                    single-frame transient fog_integrated.
//   fog_composite  — apply fog_integrated over the scene color.
//
// With each dispatch as its own RenderPass, the FrameGraph emits one
// merged barrier per image per node and the single-frame fog_integrated
// volume's lifetime collapses to [integrate..composite] (alias-eligible
// with other single-frame transients of matching shape).
//
// Layering:
//   • FogCore          — abstract base. Owns sampler, four descriptor
//                        layouts, four pipelines, scene parameters, the
//                        per-viewport Images struct (scatter ping-pong),
//                        and prepare_frame() to source scene state. Itself
//                        a RenderPass, registered as "fog_inject" through
//                        FogInjectPass.
//   • FogInjectPass    — concrete FogCore. Advances the ping-pong counter
//                        at declare time so followers see a consistent
//                        (cur, prev) pair for the whole frame. Runs the
//                        inject dispatch.
//   • FogTemporalPass,
//     FogIntegratePass,
//     FogCompositePass — followers. Hold FogCore* set in post_create()
//                        via passes.get<FogCore>("fog_inject"). Each
//                        records exactly one dispatch.
//
// Resource flow:
//   • scatter[0] / scatter[1] are persistent per-viewport Images (cross-
//     frame temporal reuse). RenderPipeline::execute injects them as
//     "fog_scatter_cur" / "fog_scatter_prev" into DeclareContext.imports
//     each frame based on the active viewport's ping-pong state, same
//     pattern Hi-Z and GTAO use for their per-viewport images.
//   • fog_integrated is a graph transient: FogIntegratePass create()s it,
//     FogCompositePass create()s it again for the dedup-by-name read.
//
// Barrier contract: nodes declare only the accesses they perform. The
// graph emits one merged barrier per image per node. No inline barriers
// — scatter layout state carries across frames through the graph's
// per-slice tracker, and fog_integrated is fully owned by the graph.

class FogCore : public RenderPass {
public:
    static constexpr uint32_t TILE_SIZE = 8;
    static constexpr uint32_t DEPTH_SLICES = 128;
    static constexpr uint32_t SCREEN_DIVISOR = 16;
    static constexpr VkFormat VOLUME_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;

    struct VolumeImage {
        VkImage image{VK_NULL_HANDLE};
        VmaAllocation memory{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
    };

    struct Images {
        VolumeImage scatter_volumes[2];

        VkExtent3D volume_extent{};
        uint32_t frame_counter{0};
        uint32_t ping_pong{0};  // index of the "current" volume this frame

        void create(VkDevice device, VmaAllocator allocator,
                     uint32_t vol_w, uint32_t vol_h);
        void destroy(VkDevice device, VmaAllocator allocator);
        void resize(VkDevice device, VmaAllocator allocator,
                     uint32_t screen_w, uint32_t screen_h);

        void invalidate_imgui_texture();

        [[nodiscard]] uint32_t cur_index() const { return ping_pong; }
        [[nodiscard]] uint32_t prev_index() const { return 1 - ping_pong; }
    };

    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;
    void prepare_frame(const FrameContext& ctx, GpuContext& gpu) override;

    void resize(uint32_t width, uint32_t height);

    [[nodiscard]] bool enabled() const { return enabled_; }

    /// The catalog name the composite reads depth under: the scene depth with
    /// the sea laid over it, which is the scene depth itself where no water
    /// draws. The composite lays fog over a pixel for the distance to what is
    /// seen there, and over the sea that is the surface — read against the
    /// opaque scene alone it fogs the water for the distance to the bed.
    static constexpr const char* COMPOSITE_DEPTH_NAME = "surface_depth";

    // Getters for follower passes.
    [[nodiscard]] VkSampler fog_sampler() const { return linear_sampler_; }
    [[nodiscard]] VkDescriptorSetLayout inject_set_layout() const { return inject_set_layout_; }
    [[nodiscard]] VkDescriptorSetLayout temporal_set_layout() const { return temporal_set_layout_; }
    [[nodiscard]] VkDescriptorSetLayout integrate_set_layout() const { return integrate_set_layout_; }
    [[nodiscard]] VkDescriptorSetLayout composite_set_layout() const { return composite_set_layout_; }
    [[nodiscard]] VkPipelineLayout inject_pipeline_layout() const { return inject_pipeline_layout_; }
    [[nodiscard]] VkPipelineLayout temporal_pipeline_layout() const { return temporal_pipeline_layout_; }
    [[nodiscard]] VkPipelineLayout integrate_pipeline_layout() const { return integrate_pipeline_layout_; }
    [[nodiscard]] VkPipelineLayout composite_pipeline_layout() const { return composite_pipeline_layout_; }
    /// The compute pipeline built from this frame's fog shader, or null when
    /// neither the node's material nor the engine's default has compiled.
    [[nodiscard]] VkPipeline inject_pipeline() const { return inject_pipeline_; }
    [[nodiscard]] VkPipeline temporal_pipeline() const { return temporal_pipeline_; }
    [[nodiscard]] VkPipeline integrate_pipeline() const { return integrate_pipeline_; }
    [[nodiscard]] VkPipeline composite_pipeline() const { return composite_pipeline_; }
    [[nodiscard]] Images& images() { return default_images_; }

    // Scene-driven fog parameters + push-constant helper read by each node.
    // Mirrors shaders/include/fjell_fog_push.glsl field for field; the four
    // fog shaders all declare the block from that one include.
    struct PushConstants {
        glm::uvec4 volume_dims;   // xyz = froxel grid, w = frame_index
        float near_plane;
        float far_plane;
        float temporal_blend;
        uint32_t viewport_width;
        uint32_t viewport_height;
        float jitter_x;
        float jitter_y;
        float jitter_z;
        // The fog node's world position: ORIGIN in the shader's density().
        float origin_x;
        float origin_y;
        float origin_z;
        // How many entries of fog_volume_ssbo the inject pass should read.
        // The other three fog shaders share this block and ignore it.
        uint32_t volume_count;
        // How far the height fog answers things moving through it. Read by
        // the inject pass; shared with the other three the same way.
        float motion_response;
        // Paints the stirred air so the density field can be seen. 0 is off.
        float density_field_debug;
        // The fog's row in the material buffer: the pass's own copy of the
        // node's material, holding this frame's resolved uniform values.
        uint32_t fog_material_id;
    };
    [[nodiscard]] PushConstants make_push(const Images& imgs,
                                           uint32_t width, uint32_t height) const;

private:
    static VolumeImage create_volume_image(VkDevice device, VmaAllocator allocator,
                                            uint32_t w, uint32_t h, uint32_t d);
    static void destroy_volume_image(VkDevice device, VmaAllocator allocator, VolumeImage& img);

    /// Build, or fetch, the compute pipeline for a fog shader entry, keeping
    /// it current with the entry's compile generation so a hot reload reaches
    /// it. Null when the entry is not a fog shader or has never compiled.
    VkPipeline ensure_material_pipeline(uint32_t pipeline_id);
    /// Register the engine's fog.fjsl and seed a material row with its
    /// defaults, for a node with no material or one that cannot draw.
    void ensure_fallback_material();
    /// Keep the pass's own material row a copy of the fog's source row, and
    /// its params slice the frame's resolved uniform values. The source row
    /// is the material's and is never written: a track folded in here must
    /// not reach the asset the inspector shows and the scene saves.
    void apply_material(uint32_t pipeline_id, uint32_t source_material_id,
                        const std::vector<NamedValue>& uniforms);
    void destroy_material_pipelines();

    // Scene-driven fog parameters (set by prepare_frame)
    bool enabled_{false};
    glm::vec3 origin_{0.0f};
    float max_distance_{130.0f};
    float temporal_blend_{0.9f};
    float motion_response_{0.0f};
    float density_field_debug_{0.0f};
    uint32_t volume_count_{0};

    PipelineCache* pipeline_cache_{nullptr};
    BindlessTextures* bindless_{nullptr};
    DeferredDeleter* deferred_deleter_{nullptr};

    struct MaterialPipeline {
        VkPipeline pipeline{VK_NULL_HANDLE};
        uint32_t generation{0};
    };
    std::unordered_map<uint32_t, MaterialPipeline> material_pipelines_;
    // Entries already complained about, so a wrong-typed material is one
    // line in the log rather than one per frame.
    std::unordered_set<uint32_t> reported_entries_;

    bool fallback_ready_{false};
    uint32_t fallback_pipeline_id_{0};
    uint32_t fallback_material_id_{~0u};

    // The pass's own row and params slice. Taken once and kept: nothing hands
    // a row back, so a fresh one per rebuild would leak. The slice is only
    // retaken when a shader wants more room than the last one had.
    uint32_t instance_material_id_{~0u};
    uint32_t instance_param_offset_{0};
    uint32_t instance_param_count_{0};
    uint32_t instance_source_material_{~0u};
    uint32_t instance_pipeline_id_{~0u};
    uint32_t instance_generation_{0};
    std::vector<NamedValue> applied_uniforms_;
    bool uniforms_applied_{false};

    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineCache vk_pipeline_cache_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};

    VkSampler linear_sampler_{VK_NULL_HANDLE};

    VkDescriptorSetLayout inject_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout inject_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline inject_pipeline_{VK_NULL_HANDLE};

    VkDescriptorSetLayout temporal_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout temporal_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline temporal_pipeline_{VK_NULL_HANDLE};

    VkDescriptorSetLayout integrate_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout integrate_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline integrate_pipeline_{VK_NULL_HANDLE};

    VkDescriptorSetLayout composite_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout composite_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline composite_pipeline_{VK_NULL_HANDLE};

    Images default_images_;
};

// Concrete inject node — IS the FogCore. Records inject dispatch, also
// advances the ping-pong counter at declare time so all four nodes see
// the same (cur, prev) pair for the whole frame.
class FogInjectPass : public FogCore {
public:
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void record(const FrameContext& ctx) override;
};

class FogTemporalPass : public RenderPass {
public:
    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;

private:
    FogCore* core_{nullptr};
};

class FogIntegratePass : public RenderPass {
public:
    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;

private:
    FogCore* core_{nullptr};
};

class FogCompositePass : public RenderPass {
public:
    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;

private:
    FogCore* core_{nullptr};
};

// Deprecated type alias: PostProcessResources still refers to
// "VolumetricFogPass" in several callers. Point them at the new
// abstract FogCore so the Images + layouts stay reachable through a
// single type everyone agrees on.
using VolumetricFogPass = FogCore;

} // namespace fjell
