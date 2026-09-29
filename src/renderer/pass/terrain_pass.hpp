#pragma once

#include "gpu/owned.hpp"
#include "gpu/pipeline.hpp"
#include "gpu/render_encoder.hpp"
#include "gpu/sampler.hpp"
#include "renderer/pass/render_pass.hpp"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace fjell {

class BindlessTextures;
class PipelineCache;
struct TerrainComponent;

// Mirrors the push block in terrain.task / terrain.mesh and the terrain FJSL
// template. Only the fragment stage reads past `view_index`.
struct TerrainPushConstants {
    glm::vec2 world_origin;
    float world_size;
    float height_scale;
    float height_offset;
    uint32_t patches_per_side;
    uint32_t patch_quads;
    /// Which projection the draw writes into: 0 the camera, 1..3 the shadow
    /// cascade of that index minus one. The cascade matrices are already in
    /// the global UBO, so this costs an index rather than a second matrix in
    /// a block every stage carries.
    uint32_t view_index;
    uint32_t terrain_material_id;   ///< material row of the terrain's own .fjmat
    uint32_t pad0;                  ///< std430 aligns the vec2 below to 8
    /// Render target size in pixels, for projecting a patch to screen height.
    glm::vec2 viewport;
    /// Projected patch height in pixels below which each coarser LOD level
    /// takes over, descending.
    glm::vec3 lod_thresholds;
    uint32_t pad3;                  ///< the uvec4 below is 16-byte aligned
    glm::uvec4 layer_material_ids;  ///< material row per layer
    glm::vec4 layer_uv_scales;      ///< metres per texture repeat, per layer
};
// Offsets taken from the compiled SPIR-V rather than assumed: std430 aligns
// the vec2 to 8 and the vec3 to 16, which C++ does not do on its own — hence
// the explicit pads.
static_assert(sizeof(TerrainPushConstants) == 96, "push block layout must match the shaders");
static_assert(offsetof(TerrainPushConstants, viewport) == 40);
static_assert(offsetof(TerrainPushConstants, lod_thresholds) == 48);
static_assert(offsetof(TerrainPushConstants, layer_material_ids) == 64);
static_assert(offsetof(TerrainPushConstants, layer_uv_scales) == 80);

/// Draws heightfield terrain. Geometry is generated in the mesh shader from the
/// asset's height texture, so this pass binds a texture and dispatches one task
/// workgroup per patch — there are no vertex or index buffers involved.
///
/// Shading comes from a `shader_type terrain` FJSL material on the node,
/// compiled through the PipelineCache like every other material. The cache
/// builds no pipelines for that type — the pipeline shape is this pass's,
/// task + mesh + FJSL fragment — so the pass builds one pipeline per terrain
/// shader it meets and rebuilds it when the shader recompiles.
///
/// Terrain is part of the depth prepass, the shadow cascades and the opaque
/// colour, so it draws inside those passes' render scopes rather than in one
/// of its own: they call `draw_depth`, `draw_shadow` and `draw`.
class TerrainPass : public RenderPass {
public:
    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;
    void prepare_frame(const FrameContext& ctx, GpuContext& gpu) override;
    /// Nothing: the pass is never scheduled, and draws where it is called.
    void record(const FrameContext& ctx) override;

    /// Draws terrain lit, into the opaque pass's targets, and the patch grid
    /// over the nodes that ask for it.
    void draw(gpu::RenderEncoder& pass, const FrameContext& ctx);

    /// Draws terrain into the depth prepass. Terrain is opaque geometry, and
    /// the colour pass tests against the prepass depth rather than writing
    /// it — without this the surface fails its own depth test wherever the
    /// prepass left the cleared far value, and terrain flickers out as the
    /// camera turns. Also feeds Hi-Z, so terrain occludes what is behind it.
    void draw_depth(gpu::RenderEncoder& pass, const FrameContext& ctx);

    /// Draws terrain into one shadow cascade. Terrain is the ground every
    /// other caster stands on, so without this a hill shadows nothing — not
    /// the character walking behind it, and not the sun shafts, which read the
    /// same cascades. `cascade` selects the light matrix from the global UBO.
    void draw_shadow(gpu::RenderEncoder& pass, const FrameContext& ctx, uint32_t cascade);

    /// Whether the scene holds a terrain with a resolved asset. Depth passes
    /// early-out on an empty render queue, which terrain is never part of, so
    /// they ask this before deciding there is nothing to draw.
    [[nodiscard]] static bool scene_has_terrain(const FrameContext& ctx);

private:
    /// Which pipeline a walk over the terrain nodes draws with.
    enum class Variant : uint8_t {
        color,       ///< the node's terrain material, lit
        depth,       ///< depth only, for the prepass
        shadow,      ///< depth only, into a shadow cascade
        wireframe,   ///< patch grid overlay, only on nodes that ask for it
    };

    /// A colour pipeline built from one terrain FJSL entry.
    struct MaterialPipeline {
        gpu::Owned<gpu::GraphicsPipeline> pipeline;
        uint32_t generation{0};   ///< PipelineCache generation it was built at
    };

    /// Build (or rebuild after a recompile) the colour pipeline for a terrain
    /// FJSL entry. Invalid when the entry failed to compile or is not a
    /// terrain shader, in which case the fallback pipeline draws instead.
    gpu::GraphicsPipeline ensure_material_pipeline(uint32_t pipeline_id);

    /// The colour pipeline and material row a node draws with: its own
    /// material when that is a terrain shader that compiled, else the engine's
    /// terrain.fjsl at its defaults.
    void resolve_node_material(const TerrainComponent& comp, gpu::GraphicsPipeline& out_pipeline,
                               uint32_t& out_material_id);

    /// Register the engine's terrain.fjsl and give it a material row, so a
    /// node with no material of its own still draws.
    void ensure_fallback_material();

    /// Walks terrain nodes, binds their textures, pushes constants, and
    /// dispatches one task workgroup per patch. `view_index` picks the
    /// projection the draw writes into: 0 the camera, 1..3 the shadow cascade
    /// of that index minus one. `extent` is the size of what is drawn into,
    /// in pixels, which each patch's level of detail is chosen against.
    void draw_terrain_nodes(gpu::RenderEncoder& pass, const FrameContext& ctx, Variant variant,
                            uint32_t view_index, glm::uvec2 extent);

    gpu::Device* device_{nullptr};
    PassResources resources_{};
    gpu::Owned<gpu::GraphicsPipeline> depth_pipeline_;
    gpu::Owned<gpu::GraphicsPipeline> shadow_pipeline_;
    gpu::Owned<gpu::GraphicsPipeline> wireframe_pipeline_;
    /// Linear and clamped to the edge, for all three terrain textures.
    gpu::Sampler sampler_{};

    /// Projected patch height in pixels below which each coarser level takes
    /// over. A patch is finest above the first, coarsest below the last.
    /// Tuned by eye against a real terrain; exposed so it can be moved
    /// without a shader recompile.
    glm::vec3 lod_thresholds_{220.0F, 90.0F, 35.0F};

    PipelineCache* pipeline_cache_{nullptr};
    BindlessTextures* bindless_{nullptr};
    std::unordered_map<uint32_t, MaterialPipeline> material_pipelines_;
    /// Entries already reported as unusable, so the log says it once.
    std::unordered_set<uint32_t> reported_entries_;
    uint32_t fallback_pipeline_id_{0};
    uint32_t fallback_material_id_{0};
    bool fallback_ready_{false};
};

} // namespace fjell
