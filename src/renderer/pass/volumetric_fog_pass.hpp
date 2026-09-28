#pragma once

#include "renderer/pass/render_pass.hpp"
#include "gpu/format.hpp"
#include "gpu/owned.hpp"
#include "gpu/texture.hpp"

#include <glm/glm.hpp>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <string_view>
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
// Fog runs five compute steps authored as five independent DAG nodes:
//
//   fog_medium     — one dispatch per source of fog into the single-frame
//                    transient fog_medium: the haze over the whole froxel
//                    grid, then each placed volume over the froxels its
//                    bounds reach. Every source is a `shader_type fog`
//                    material compiled through PipelineCache into the fog
//                    template's medium variant; the engine's fog.fjsl and
//                    fog_volume.fjsl stand in for a node with no material.
//   fog_light      — the light variant of the haze's material, once over
//                    the grid: reads fog_medium, applies the stir and the
//                    distance fade, scatters every light through light()
//                    into scatter[cur].
//   fog_temporal   — blend scatter[cur] with scatter[prev] (the last
//                    frame's result). Reads scatter[prev], reads+writes
//                    scatter[cur].
//   fog_integrate  — front-to-back ray march scatter[cur] into the
//                    single-frame transient fog_integrated.
//   fog_composite  — apply fog_integrated over the scene color.
//
// The first four run in the Color phase, between the water's depth
// prepass and the water, which samples fog_integrated for the water behind
// its surface; the composite runs in PostProcess, after the transparents.
// With each dispatch as its own RenderPass, the FrameGraph emits one merged
// barrier per image per node, and the single-frame transients collapse to
// [medium..light] and [integrate..composite] (alias-eligible with other
// single-frame transients of matching shape).
//
// Layering:
//   • FogCore          — abstract base. Owns sampler, descriptor layouts,
//                        the engine's own pipelines, the per-material fog
//                        pipelines and rows, scene parameters, the
//                        per-viewport Images struct (scatter ping-pong),
//                        and prepare_frame() to source scene state. Itself
//                        a RenderPass, registered as "fog_medium" through
//                        FogMediumPass.
//   • FogMediumPass    — concrete FogCore. Records the medium dispatches.
//   • FogLightPass,
//     FogTemporalPass,
//     FogIntegratePass,
//     FogCompositePass — followers. Hold FogCore* set in post_create()
//                        via passes.get<FogCore>("fog_medium"). Each
//                        records exactly one dispatch.
//
// Resource flow:
//   • scatter[0] / scatter[1] are persistent per-viewport Images (cross-
//     frame temporal reuse). RenderPipeline::execute injects them as
//     "fog_scatter_cur" / "fog_scatter_prev" into DeclareContext.imports
//     each frame based on the active viewport's ping-pong state, same
//     pattern Hi-Z and GTAO use for their per-viewport images.
//   • fog_medium and fog_integrated are graph transients: the writer
//     create()s each, the reader create()s it again for the dedup-by-name
//     read.
//
// Barrier contract: nodes declare only the accesses they perform. The
// graph emits one merged barrier per image per node. The one inline
// barrier is the medium pass's own, between its dispatches, because they
// accumulate into one image inside one node.

class FogCore : public RenderPass {
public:
    static constexpr uint32_t TILE_SIZE = 8;
    static constexpr uint32_t DEPTH_SLICES = 128;
    static constexpr uint32_t SCREEN_DIVISOR = 16;
    static constexpr gpu::Format VOLUME_FORMAT = gpu::Format::rgba16_float;

    struct VolumeImage {
        VkImage image{VK_NULL_HANDLE};
        VmaAllocation memory{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
        /// The volume as the GPU interface's texture.
        gpu::Owned<gpu::Texture> texture;
    };

    struct Images {
        VolumeImage scatter_volumes[2];

        VkExtent3D volume_extent{};
        uint32_t frame_counter{0};
        uint32_t ping_pong{0};  // index of the "current" volume this frame

        void create(gpu::Device& gpu, VkDevice device, VmaAllocator allocator,
                     uint32_t screen_w, uint32_t screen_h);
        void destroy(VkDevice device, VmaAllocator allocator);
        void resize(gpu::Device& gpu, VkDevice device, VmaAllocator allocator,
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
    /// The catalog name of the accumulated medium the medium pass writes and
    /// the light pass reads.
    static constexpr const char* MEDIUM_NAME = "fog_medium";
    /// The catalog name of the fog integrated front to back: what the
    /// composite lays over the scene, and what the water samples for the
    /// water behind its surface.
    static constexpr const char* INTEGRATED_NAME = "fog_integrated";
    /// The froxel grid, the shape of every transient in the chain; a reader
    /// re-creating one by name passes this.
    [[nodiscard]] static TextureDesc volume_desc();
    /// Where the froxel slices begin, in metres of view depth. They end at
    /// far_plane() and are spaced exponentially between the two.
    static constexpr float NEAR_PLANE = 0.1F;
    [[nodiscard]] float far_plane() const { return max_distance_; }
    /// The catalog name of the waterline mask: one texel per froxel column,
    /// 1 where the column begins on the other side of a water's surface from
    /// the camera. Written by the water pass at the medium node's request.
    static constexpr const char* WATER_LINE_NAME = "water_line";

    // Getters for follower passes.
    [[nodiscard]] VkSampler fog_sampler() const { return linear_sampler_; }
    [[nodiscard]] VkDescriptorSetLayout medium_set_layout() const { return medium_set_layout_; }
    [[nodiscard]] VkDescriptorSetLayout light_set_layout() const { return light_set_layout_; }
    [[nodiscard]] VkDescriptorSetLayout temporal_set_layout() const { return temporal_set_layout_; }
    [[nodiscard]] VkDescriptorSetLayout integrate_set_layout() const { return integrate_set_layout_; }
    [[nodiscard]] VkDescriptorSetLayout composite_set_layout() const { return composite_set_layout_; }
    [[nodiscard]] VkPipelineLayout medium_pipeline_layout() const { return medium_pipeline_layout_; }
    [[nodiscard]] VkPipelineLayout light_pipeline_layout() const { return light_pipeline_layout_; }
    [[nodiscard]] VkPipelineLayout temporal_pipeline_layout() const { return temporal_pipeline_layout_; }
    [[nodiscard]] VkPipelineLayout integrate_pipeline_layout() const { return integrate_pipeline_layout_; }
    [[nodiscard]] VkPipelineLayout composite_pipeline_layout() const { return composite_pipeline_layout_; }
    /// The haze material's two pipelines for this frame, or null when
    /// neither the node's material nor the engine's default has compiled.
    [[nodiscard]] VkPipeline medium_pipeline() const { return medium_pipeline_; }
    [[nodiscard]] VkPipeline light_pipeline() const { return light_pipeline_; }
    [[nodiscard]] VkPipeline temporal_pipeline() const { return temporal_pipeline_; }
    [[nodiscard]] VkPipeline integrate_pipeline() const { return integrate_pipeline_; }
    [[nodiscard]] VkPipeline composite_pipeline() const { return composite_pipeline_; }
    [[nodiscard]] Images& images() { return default_images_; }

    // Scene-driven fog parameters + push-constant helper read by each node.
    // Mirrors shaders/include/fjell_fog_push.glsl field for field; the fog
    // shaders all declare the block from that one include.
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
        // The source's node position: ORIGIN in the shader's density().
        float origin_x;
        float origin_y;
        float origin_z;
        // HAZE for the scene's fog, else the index of the volume a medium
        // dispatch adds. The other steps share this block and ignore it.
        uint32_t volume_index;
        // How far the haze answers things moving through it.
        float motion_response;
        // Paints the stirred air so the density field can be seen. 0 is off.
        float density_field_debug;
        // The source's row in the material buffer: the pass's own copy of
        // the node's material, holding this frame's resolved uniform values.
        uint32_t fog_material_id;
        // Which kind of source this dispatch adds: SOURCE_HAZE, SOURCE_VOLUME
        // or SOURCE_WATER.
        uint32_t source_kind;
        // For a water source: whether the camera is under its surface, which
        // decides which side of the surface depth the water is on.
        uint32_t camera_below;
    };
    static constexpr uint32_t SOURCE_HAZE = 0;
    static constexpr uint32_t SOURCE_VOLUME = 1;
    static constexpr uint32_t SOURCE_WATER = 2;
    static constexpr uint32_t HAZE = ~0u;

    /// The block as the haze fills it; a volume dispatch overwrites the
    /// source fields.
    [[nodiscard]] PushConstants make_push(const Images& imgs,
                                           uint32_t width, uint32_t height) const;

    /// One placed volume the medium pass draws this frame, resolved to what
    /// the dispatch needs.
    struct VolumeDraw {
        uint32_t index{0};          // its entry in the fog volume SSBO
        VkPipeline pipeline{VK_NULL_HANDLE};
        uint32_t material_id{0};    // the pass's row for this volume
        glm::mat4 transform{1.0f};  // world, for the froxel box and ORIGIN
    };
    [[nodiscard]] const std::vector<VolumeDraw>& volume_draws() const { return volume_draws_; }

    /// One body of water the medium pass draws this frame. Over the whole
    /// grid: which froxels are inside it is told from the surface depth in
    /// the shader, not from a box on the CPU.
    struct WaterDraw {
        uint32_t node_id{0};
        VkPipeline pipeline{VK_NULL_HANDLE};
        uint32_t material_id{0};    // the pass's row for this water
        glm::vec3 origin{0.0f};     // the node's position: ORIGIN
    };
    [[nodiscard]] const std::vector<WaterDraw>& water_draws() const { return water_draws_; }

private:
    static VolumeImage create_volume_image(gpu::Device& gpu, VkDevice device, VmaAllocator allocator,
                                            uint32_t w, uint32_t h, uint32_t d);
    static void destroy_volume_image(VkDevice device, VmaAllocator allocator, VolumeImage& img);

    /// The medium and light pipelines built from one fog shader entry.
    struct MaterialPipeline {
        VkPipeline medium{VK_NULL_HANDLE};
        VkPipeline light{VK_NULL_HANDLE};
        uint32_t generation{0};
    };
    /// The engine's default for a kind of node, registered once and given a
    /// row seeded with the shader's own defaults.
    struct Fallback {
        bool ready{false};
        uint32_t pipeline_id{0};
        uint32_t material_id{~0u};
    };
    /// The pass's own row for one source. Taken once and kept: nothing hands
    /// a row back, so a fresh one per rebuild would leak. The slice is only
    /// retaken when a shader wants more room than the last one had.
    struct Instance {
        uint32_t material_id{~0u};
        uint32_t param_offset{0};
        uint32_t param_count{0};
        uint32_t source_material{~0u};
        uint32_t pipeline_id{~0u};
        uint32_t generation{0};
        std::vector<NamedValue> applied_uniforms;
        bool applied{false};
    };

    /// Build, or fetch, both pipelines for a fog shader entry, keeping them
    /// current with the entry's compile generation so a hot reload reaches
    /// them. Null when the entry is not a fog shader or has never compiled.
    const MaterialPipeline* ensure_material_pipeline(uint32_t pipeline_id);
    /// Register an engine default and seed a material row with its defaults,
    /// for a node with no material or one that cannot draw.
    void ensure_fallback(Fallback& fallback, std::string_view shader);
    /// The pipelines and source row a source draws with: its own material
    /// when that compiles, else the kind's fallback. Null when neither does.
    const MaterialPipeline* resolve_source(uint32_t& pipeline_id, uint32_t& material_id,
                                           const Fallback& fallback);
    /// Keep an instance row a copy of its source row, and its params slice
    /// the frame's resolved uniform values. The source row is the material's
    /// and is never written: a track folded in here must not reach the asset
    /// the inspector shows and the scene saves.
    void apply_instance(Instance& instance, uint32_t pipeline_id, uint32_t source_material_id,
                        const std::vector<NamedValue>& uniforms);
    void destroy_material_pipelines();

    // Scene-driven fog parameters (set by prepare_frame)
    bool enabled_{false};
    glm::vec3 origin_{0.0f};
    float max_distance_{130.0f};
    float temporal_blend_{0.9f};
    float motion_response_{0.0f};
    float density_field_debug_{0.0f};

    PipelineCache* pipeline_cache_{nullptr};
    BindlessTextures* bindless_{nullptr};
    DeferredDeleter* deferred_deleter_{nullptr};

    std::unordered_map<uint32_t, MaterialPipeline> material_pipelines_;
    // Entries already complained about, so a wrong-typed material is one
    // line in the log rather than one per frame.
    std::unordered_set<uint32_t> reported_entries_;

    Fallback haze_fallback_;
    Fallback volume_fallback_;
    Fallback water_fallback_;
    Instance haze_instance_;
    // By node id, so a volume keeps its row across frames. A volume that
    // leaves the scene leaves its row behind, as the bindless registry is
    // append-only; a handful of orphaned floats per removed volume.
    std::unordered_map<uint32_t, Instance> volume_instances_;
    std::vector<VolumeDraw> volume_draws_;
    std::unordered_map<uint32_t, Instance> water_instances_;
    std::vector<WaterDraw> water_draws_;

    VkDevice device_{VK_NULL_HANDLE};
    gpu::Device* gpu_device_{nullptr};
    VkPipelineCache vk_pipeline_cache_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};

    VkSampler linear_sampler_{VK_NULL_HANDLE};

    VkDescriptorSetLayout medium_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout medium_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline medium_pipeline_{VK_NULL_HANDLE};

    VkDescriptorSetLayout light_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout light_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline light_pipeline_{VK_NULL_HANDLE};

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

// Concrete medium node — IS the FogCore. Records one dispatch per source of
// fog into the accumulated medium.
class FogMediumPass : public FogCore {
public:
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void record(const FrameContext& ctx) override;
};

class FogLightPass : public RenderPass {
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
