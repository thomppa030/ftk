#pragma once

#include "renderer/pass/render_pass.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>

namespace fjell {

class TerrainAsset;

// Mirrors the push block in terrain.task / terrain.mesh.
struct TerrainPushConstants {
    glm::vec2 world_origin;
    float world_size;
    float height_scale;
    float height_offset;
    uint32_t patches_per_side;
    uint32_t patch_quads;
    uint32_t pad;
};

/// Draws heightfield terrain. Geometry is generated in the mesh shader from the
/// asset's height texture, so this pass binds a texture and dispatches one task
/// workgroup per patch — there are no vertex or index buffers involved.
class TerrainPass : public RenderPass {
public:
    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
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
    /// Point the descriptor at a terrain's height texture. Cheap to call every
    /// frame: it early-outs unless the asset changed.
    void bind_height_texture(const TerrainAsset& terrain);

    /// Shared by record() and record_depth(): walks terrain nodes, binds the
    /// height texture, pushes constants, and dispatches one task workgroup per
    /// patch through `pipeline` / `layout`.
    void draw_terrain_nodes(const FrameContext& ctx, VkPipeline pipeline,
                            VkPipelineLayout layout, bool count_draws);

    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};
    VkPipeline depth_pipeline_{VK_NULL_HANDLE};
    VkDescriptorSetLayout height_set_layout_{VK_NULL_HANDLE};
    VkDescriptorPool descriptor_pool_{VK_NULL_HANDLE};
    VkDescriptorSet height_set_{VK_NULL_HANDLE};
    const TerrainAsset* bound_terrain_{nullptr};
    PFN_vkCmdDrawMeshTasksEXT draw_mesh_tasks_{nullptr};
};

} // namespace fjell
