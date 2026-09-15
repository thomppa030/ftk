#pragma once

#include "renderer/pass/render_pass.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace fjell {

class BindlessTextures;
class PipelineCache;
class TerrainAsset;
struct TerrainComponent;

// Mirrors the push block in terrain.task / terrain.mesh and the terrain FJSL
// template. Only the fragment stage reads past `pad`.
struct TerrainPushConstants {
    glm::vec2 world_origin;
    float world_size;
    float height_scale;
    float height_offset;
    uint32_t patches_per_side;
    uint32_t patch_quads;
    uint32_t pad;
    uint32_t terrain_material_id;   ///< material row of the terrain's own .fjmat
    uint32_t pad1;                  ///< the uvec4 below is 16-byte aligned
    uint32_t pad2;
    uint32_t pad3;
    glm::uvec4 layer_material_ids;  ///< material row per layer
    glm::vec4 layer_uv_scales;      ///< metres per texture repeat, per layer
};
static_assert(sizeof(TerrainPushConstants) == 80, "push block layout must match the shaders");
static_assert(offsetof(TerrainPushConstants, layer_material_ids) == 48);
static_assert(offsetof(TerrainPushConstants, layer_uv_scales) == 64);

/// Draws heightfield terrain. Geometry is generated in the mesh shader from the
/// asset's height texture, so this pass binds a texture and dispatches one task
/// workgroup per patch — there are no vertex or index buffers involved.
///
/// Shading comes from a `shader_type terrain` FJSL material on the node,
/// compiled through the PipelineCache like every other material. The cache
/// builds no pipelines for that type — the pipeline shape is this pass's,
/// task + mesh + FJSL fragment — so the pass builds one VkPipeline per terrain
/// shader it meets and rebuilds it when the shader recompiles.
class TerrainPass : public RenderPass {
public:
    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;
    void prepare_frame(const FrameContext& ctx, GpuContext& gpu) override;
    void record(const FrameContext& ctx) override;

    /// Record terrain into the depth prepass. Terrain is opaque geometry, and
    /// the color pass loads depth with COMPARE_OP_LESS rather than writing it
    /// from scratch — without this the surface fails its own depth test wherever
    /// the prepass left the cleared far value, and terrain flickers out as the
    /// camera turns. Also feeds Hi-Z, so terrain occludes what is behind it.
    void record_depth(const FrameContext& ctx);

    /// Whether the scene holds a terrain with a resolved asset. Depth passes
    /// early-out on an empty render queue, which terrain is never part of, so
    /// they ask this before deciding there is nothing to draw.
    [[nodiscard]] static bool scene_has_terrain(const FrameContext& ctx);

private:
    /// Which pipeline a walk over the terrain nodes draws with.
    enum class Variant : uint8_t {
        color,       ///< the node's terrain material, lit
        depth,       ///< depth only, for the prepass
        wireframe,   ///< patch grid overlay, only on nodes that ask for it
    };

    /// A colour pipeline built from one terrain FJSL entry.
    struct MaterialPipeline {
        VkPipeline pipeline{VK_NULL_HANDLE};
        uint32_t generation{0};   ///< PipelineCache generation it was built at
    };

    /// Point the descriptor set at a terrain's height and weight textures.
    /// Cheap to call every frame: it early-outs unless the asset changed.
    void bind_terrain_textures(const TerrainAsset& terrain);

    /// Build (or rebuild after a recompile) the colour pipeline for a terrain
    /// FJSL entry. Returns null when the entry failed to compile or is not a
    /// terrain shader, in which case the fallback pipeline draws instead.
    VkPipeline ensure_material_pipeline(uint32_t pipeline_id);

    /// The colour pipeline and material row a node draws with: its own
    /// material when that is a terrain shader that compiled, else the engine's
    /// terrain.fjsl at its defaults.
    void resolve_node_material(const TerrainComponent& comp, VkPipeline& out_pipeline,
                               uint32_t& out_material_id);

    /// Register the engine's terrain.fjsl and give it a material row, so a
    /// node with no material of its own still draws.
    void ensure_fallback_material();

    /// Walks terrain nodes, binds their textures, pushes constants, and
    /// dispatches one task workgroup per patch.
    void draw_terrain_nodes(const FrameContext& ctx, Variant variant, bool count_draws);

    void destroy_material_pipelines();

    VkDevice device_{VK_NULL_HANDLE};
    PassResources resources_{};
    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline depth_pipeline_{VK_NULL_HANDLE};
    VkPipeline wireframe_pipeline_{VK_NULL_HANDLE};
    /// Kept alive after create(): every material pipeline is built from them.
    VkShaderModule task_module_{VK_NULL_HANDLE};
    VkShaderModule mesh_module_{VK_NULL_HANDLE};
    VkDescriptorSetLayout terrain_set_layout_{VK_NULL_HANDLE};
    VkDescriptorPool descriptor_pool_{VK_NULL_HANDLE};
    VkDescriptorSet terrain_set_{VK_NULL_HANDLE};
    const TerrainAsset* bound_terrain_{nullptr};
    PFN_vkCmdDrawMeshTasksEXT draw_mesh_tasks_{nullptr};

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
