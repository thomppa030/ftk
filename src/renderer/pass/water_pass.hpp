#pragma once

#include "renderer/gpu/buffer.hpp"
#include "renderer/resources/water_clipmap.hpp"
#include "renderer/resources/water_gpu.hpp"
#include "renderer/pass/render_pass.hpp"
#include "renderer/renderer_constants.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace fjell {

class BindlessTextures;
class PipelineCache;
class WaterFftPass;
struct WaterComponent;

// Mirrors the push block in water.task / water.mesh and the water FJSL
// template. Only the fragment stage reads past `patch_quads`.
struct WaterPushConstants {
    glm::vec2 world_origin;      ///< min corner of the surface, world XZ (bounded)
    float world_size;            ///< extent in metres, square (bounded)
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
    /// Nonzero when the surface is the camera-following clipmap rather than a
    /// placed square. The bounded fields above are unread in that case: an
    /// infinite sea has no corner and no size.
    uint32_t infinite;
    /// Metres the simulated sea's textures tile over, or 0 with none running.
    /// The mesh stage reads it from the field buffer; this is for the fragment
    /// stage, which has no field buffer and wants it for the tile debug view.
    float simulated_patch_size;
    /// World bounds of the scene SDF, for the fragment stage to map a world
    /// position into the volume. The same pair DdgiPass is handed, from the
    /// same `SceneSdfPass::volume_min()` / `volume_max()`, because there is
    /// one composite SDF and every reader has to agree on where it sits.
    ///
    /// Equal min and max mean there is no SDF this frame, which a reader must
    /// treat as "no geometry near" rather than dividing by the zero extent.
    glm::vec3 sdf_volume_min;
    /// This node's wakes: where its records start in the frame's wake buffer,
    /// and how many. Every water node's wakes go in one buffer per frame, so
    /// a draw is handed a range rather than the buffer rewritten under it.
    uint32_t wake_first;
    glm::vec3 sdf_volume_max;
    uint32_t wake_count;
    /// Nonzero when the camera is under this body of water: below the surface
    /// at its own position, worked out on the CPU from the wave field. The
    /// fragment stage considers a pixel's underside only then, because a
    /// folded crest turns its normal over and would otherwise read as seen
    /// from below.
    uint32_t camera_below;
    /// For the waterline dispatch alone: nonzero when a body of water is
    /// close enough to the camera for the near plane to cut its surface, so
    /// the mask is worth working out; zero writes an empty mask.
    uint32_t line_active;
    uint32_t _pad_b;
    uint32_t _pad_c;
};
static_assert(sizeof(WaterPushConstants) == 96, "push block layout must match the shaders");

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

    /// Copy the scene depth into the water's own depth image, so the sea is
    /// still hidden by whatever opaque geometry stands in front of it.
    /// Recorded by WaterDepthSeedPass, which declares both images: the scene
    /// depth must be in TRANSFER_SRC_OPTIMAL and the water's in
    /// TRANSFER_DST_OPTIMAL.
    void seed_water_depth(const FrameContext& ctx);

    /// Whether the sea draws into this target this frame: the scene holds
    /// water and the pass has what it needs to draw it. The seed, the draw and
    /// whoever offers the surface depth to the frame all ask this, because a
    /// surface depth nothing wrote is not one.
    [[nodiscard]] bool draws(const FrameContext& ctx) const;

    /// Whether the scene holds water. Nothing else in the frame knows a water
    /// node exists — it is absent from the render queue — so passes that
    /// early-out on an empty queue ask this first.
    [[nodiscard]] static bool scene_has_water(const FrameContext& ctx);

    /// Where the scene SDF sits in the world, so a water material can ask how
    /// close solid geometry is and foam against it.
    ///
    /// Pushed rather than read from the pass directly for the same reason
    /// DdgiPass takes it this way: the volume follows the camera and is
    /// rebuilt in regions, so its bounds move between frames and every reader
    /// has to be handed the current pair rather than caching one.
    void set_sdf_bounds(const glm::vec3& min, const glm::vec3& max) {
        sdf_volume_min_ = min;
        sdf_volume_max_ = max;
    }

private:
    /// A colour pipeline built from one water FJSL entry.
    struct MaterialPipeline {
        VkPipeline pipeline{VK_NULL_HANDLE};
        uint32_t generation{0};   ///< PipelineCache generation it was built at
    };

    /// Write every water node's wave field into this frame's field buffer,
    /// one record per node, and note where each landed. A node past the
    /// buffer's capacity is not drawn that frame.
    void upload_fields(const FrameContext& ctx);

    /// Write the clipmap level table into this frame's clipmap buffer.
    void upload_clipmap(const FrameContext& ctx);
    /// The water's own set for one node this frame: its field, the clipmap,
    /// the simulated sea's textures and the wakes. Per frame because those
    /// textures swap every frame, which a set written once could not
    /// follow; per node because each reads its own field record.
    [[nodiscard]] VkDescriptorSet acquire_water_set(const FrameContext& ctx,
                                                    VkDeviceSize field_offset) const;

    /// Pack every water node's wakes into this frame's wake buffer and note
    /// where each node's begin.
    void upload_wakes(const FrameContext& ctx);

public:
    /// Whether the camera is under the body of water `node_id`, as decided
    /// this frame from the wave field at the camera's position. False for a
    /// node that is not water. For the passes after this one that need the
    /// camera's side: the fog, for which froxels are inside the water.
    [[nodiscard]] bool camera_below(uint32_t node_id) const {
        const auto it = camera_gap_.find(node_id);
        return it != camera_gap_.end() && it->second < 0.0F;
    }

    /// The waterline: where the near plane cuts a surface, which pixels are
    /// on the other side of it from the camera. Written into `mask`, one
    /// texel per froxel column, for the fog to know that those columns begin
    /// in water when the camera is in air and in air when it is in water.
    /// Recorded from the fog medium pass, outside any rendering scope, once
    /// the water's buffers for the frame are in place. Writes an empty mask
    /// when no surface is within reach of the camera.
    void record_water_line(const FrameContext& ctx, VkImageView mask, VkExtent2D size);

    /// How close, in metres, a surface has to be to the camera for the
    /// waterline to be worked out. The near plane is centimetres; the slack
    /// is for a crest arriving between one frame and the next.
    static constexpr float LINE_REACH_METRES = 1.0F;

private:
    /// Per water node: how far the camera stands above its surface, negative
    /// under it.
    std::unordered_map<uint32_t, float> camera_gap_;

    /// The waterline compute pipeline: the water set as the mesh stage has
    /// it, plus the mask it writes.
    VkShaderModule line_module_{VK_NULL_HANDLE};
    VkDescriptorSetLayout line_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout line_layout_{VK_NULL_HANDLE};
    VkPipeline line_pipeline_{VK_NULL_HANDLE};

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
    /// Where the scene SDF sits, refreshed each frame through set_sdf_bounds().
    /// Equal min and max mean no SDF, which the shader reads as no geometry
    /// near rather than dividing by a zero extent.
    glm::vec3 sdf_volume_min_{0.0F};
    glm::vec3 sdf_volume_max_{0.0F};
    /// Kept alive after create(): every material pipeline is built from them.
    VkShaderModule task_module_{VK_NULL_HANDLE};
    VkShaderModule mesh_module_{VK_NULL_HANDLE};
    VkDescriptorSetLayout water_set_layout_{VK_NULL_HANDLE};
    /// The simulated sea's textures, which every water draw samples. Owned by
    /// the registry; null only if the render graph lists no water_fft pass.
    WaterFftPass* simulation_{nullptr};
    /// Whether the scene currently holds a second, different simulated sea and
    /// that has been said, so it is said when it starts and not every frame.
    bool warned_second_sea_{false};
    /// How many water nodes a frame's field buffer holds. Past it a node is
    /// not drawn that frame.
    static constexpr uint32_t WATER_NODES_PER_FRAME = 16;
    /// The wave fields the mesh shader reads, one record per water node at
    /// `field_stride_`, one buffer per frame in flight: a frame still
    /// drawing must not see the next one's field, and two nodes drawn in
    /// one frame must each read their own. Host-visible and mapped; a
    /// couple of kilobytes per node is not worth a staging copy.
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> field_buffers_;
    /// The distance between records: the field's size rounded up to the
    /// device's uniform buffer offset alignment.
    VkDeviceSize field_stride_{0};
    /// Where this frame's record for each node sits, and the one value of
    /// it the push constants also need.
    struct FieldSlot {
        VkDeviceSize offset{0};
        float simulated_patch_size{0.0F};
    };
    std::unordered_map<const WaterComponent*, FieldSlot> field_slots_;
    /// The levels an infinite surface is drawn from, recentered on the camera
    /// once per frame. One set of levels for the scene rather than one per
    /// node: they follow the camera, not the water, so two infinite seas at
    /// different heights share them. One buffer per frame in flight for the
    /// same reason as the fields.
    WaterClipmap clipmap_;
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> clipmap_buffers_;

    /// How many wake records a frame's buffer holds across every water node.
    /// Past it a node's remaining wakes are not drawn that frame.
    static constexpr uint32_t WAKE_RECORDS_PER_FRAME = 64;
    /// The wakes the mesh and fragment stages read, one buffer per frame in
    /// flight: they change every frame, and a frame still drawing must not
    /// see the next one's. Host-visible and mapped; a few kilobytes.
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> wake_buffers_;
    struct WakeRange {
        uint32_t first{0};
        uint32_t count{0};
    };
    std::unordered_map<const WaterComponent*, WakeRange> wake_ranges_;
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

/// Seeds the water's depth attachment from the finished opaque scene, as a
/// graph node of its own so the graph moves both images into the copy's
/// layouts and on to where their next readers want them.
class WaterDepthSeedPass : public RenderPass {
public:
    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;

private:
    WaterPass* water_{nullptr};
};

} // namespace fjell
