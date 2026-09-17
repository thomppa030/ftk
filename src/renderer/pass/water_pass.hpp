#pragma once

#include "renderer/gpu/buffer.hpp"
#include "renderer/resources/water_gpu.hpp"
#include "renderer/pass/render_pass.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace fjell {

class BindlessTextures;
class PipelineCache;
struct WaterComponent;

// Mirrors the push block in water.task / water.mesh and the water FJSL
// template. Only the fragment stage reads past `patch_quads`.
struct WaterPushConstants {
    glm::vec2 world_origin;      ///< min corner of the surface, world XZ
    float world_size;            ///< extent in metres (square)
    float surface_height;        ///< world Y of the surface at rest
    uint32_t patches_per_side;
    uint32_t patch_quads;
    uint32_t water_material_id;  ///< material row of the water's own .fjmat
    float delta_time;            ///< seconds since the previous frame
    /// Amplitude-weighted direction of travel of the wave field, world XZ, or
    /// (0,0) when the trains cancel. Constant across the surface, so it rides
    /// here rather than costing a varying — and the fragment stage gets it
    /// without the wave field having to reach past the mesh stage.
    glm::vec2 wave_direction;
};
static_assert(sizeof(WaterPushConstants) == 40, "push block layout must match the shaders");

/// Draws water. The surface is generated in the mesh shader from the node's
/// `.fjwater` wave field, so this pass uploads that field and dispatches one
/// task workgroup per patch — no vertex or index buffers are involved.
///
/// Shading comes from a `shader_type water` FJSL material on the node,
/// compiled through the PipelineCache like every other material. The cache
/// builds no pipelines for that type — the pipeline shape is this pass's,
/// task + mesh + FJSL fragment — so the pass builds one VkPipeline per water
/// shader it meets and rebuilds it when the shader recompiles.
///
/// Recorded from inside TransparentPass rather than as a pass of its own.
/// Water is blended and refracts what is behind it, so it has to draw after
/// the opaque scene and after the copy of it that it samples; being part of
/// that pass is what puts it there, the same way terrain belongs to the
/// opaque one.
class WaterPass : public RenderPass {
public:
    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;
    void prepare_frame(const FrameContext& ctx, GpuContext& gpu) override;
    void record(const FrameContext& ctx) override;

    /// Whether the scene holds water. Nothing else in the frame knows a water
    /// node exists — it is absent from the render queue — so passes that
    /// early-out on an empty queue ask this first.
    [[nodiscard]] static bool scene_has_water(const FrameContext& ctx);

private:
    /// A colour pipeline built from one water FJSL entry.
    struct MaterialPipeline {
        VkPipeline pipeline{VK_NULL_HANDLE};
        uint32_t generation{0};   ///< PipelineCache generation it was built at
    };

    /// Upload a node's wave field to the uniform buffer the mesh shader
    /// reads. Cheap to call every frame: it early-outs unless the field
    /// changed, which is what lets the editor drive it live.
    void bind_water_field(const WaterComponent& comp);

    /// Build (or rebuild after a recompile) the colour pipeline for a water
    /// FJSL entry. Returns null when the entry failed to compile or is not a
    /// water shader, in which case the fallback pipeline draws instead.
    VkPipeline ensure_material_pipeline(uint32_t pipeline_id);

    /// The colour pipeline and material row a node draws with: its own
    /// material when that is a water shader that compiled, else the engine's
    /// water.fjsl at its defaults.
    void resolve_node_material(const WaterComponent& comp, VkPipeline& out_pipeline,
                               uint32_t& out_material_id);

    /// Register the engine's water.fjsl and give it a material row, so a node
    /// with no material of its own still draws.
    void ensure_fallback_material();

    void destroy_material_pipelines();

    VkDevice device_{VK_NULL_HANDLE};
    PassResources resources_{};
    VkPipelineLayout layout_{VK_NULL_HANDLE};
    /// Kept alive after create(): every material pipeline is built from them.
    VkShaderModule task_module_{VK_NULL_HANDLE};
    VkShaderModule mesh_module_{VK_NULL_HANDLE};
    VkDescriptorSetLayout water_set_layout_{VK_NULL_HANDLE};
    VkDescriptorPool descriptor_pool_{VK_NULL_HANDLE};
    VkDescriptorSet water_set_{VK_NULL_HANDLE};
    /// The wave field the mesh shader reads. Host-visible and mapped: it is
    /// half a kilobyte rewritten when a sea is edited, which is not worth a
    /// staging copy.
    std::unique_ptr<Buffer> field_buffer_;
    /// Which node's field is in the buffer, and what it holds, so an
    /// unchanged one is not re-uploaded. The copy exists because the buffer's
    /// memory is write-combined and reading it back to compare is slow.
    const WaterComponent* bound_water_{nullptr};
    GpuWaterField bound_field_{};
    PFN_vkCmdDrawMeshTasksEXT draw_mesh_tasks_{nullptr};

    PipelineCache* pipeline_cache_{nullptr};
    BindlessTextures* bindless_{nullptr};
    VmaAllocator allocator_{nullptr};
    std::unordered_map<uint32_t, MaterialPipeline> material_pipelines_;
    /// Entries already reported as unusable, so the log says it once.
    std::unordered_set<uint32_t> reported_entries_;
    uint32_t fallback_pipeline_id_{0};
    uint32_t fallback_material_id_{0};
    bool fallback_ready_{false};
};

} // namespace fjell
