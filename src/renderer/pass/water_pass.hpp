#pragma once

#include "gpu/binding.hpp"
#include "gpu/owned.hpp"
#include "gpu/pipeline.hpp"
#include "gpu/render_encoder.hpp"
#include "gpu/texture.hpp"
#include "renderer/resources/water_clipmap.hpp"
#include "renderer/resources/water_gpu.hpp"
#include "renderer/pass/render_pass.hpp"
#include "renderer/renderer_constants.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fjell {

class BindlessTextures;
class FogCore;
class OffscreenPass;
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
    /// The fog's froxel volume, for the water behind the surface: where its
    /// slices begin and end in metres of view depth, and how many there are.
    /// A far of zero means no medium in this draw, and the fragment stage
    /// leaves MEDIUM_TRANSMITTANCE at one and MEDIUM_INSCATTER at zero.
    float medium_near;
    float medium_far;
    uint32_t medium_slices;
};
static_assert(sizeof(WaterPushConstants) == 100, "push block layout must match the shaders");

/// Draws water. The surface is generated in the mesh shader from the node's
/// `.fjwater` wave field, so this pass uploads that field and dispatches one
/// task workgroup per patch — no vertex or index buffers are involved.
///
/// Shading comes from a `shader_type water` FJSL material on the node,
/// compiled through the PipelineCache like every other material. The cache
/// builds no pipelines for that type — the pipeline shape is this pass's,
/// task + mesh + FJSL fragment — so the pass builds one pipeline per water
/// shader it meets and rebuilds it when the shader recompiles.
///
/// A graph node of its own, "water", just before the transparent pass. Water
/// is blended and refracts what is behind it, so it draws after the opaque
/// scene and the copy of it that it samples; the transparent pass then
/// carries on over it with the same colour targets.
class WaterPass : public RenderPass {
public:
    Result<> create(const PassResources& resources) override;
    void destroy() override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;
    void prepare_frame(const FrameContext& ctx, GpuContext& gpu) override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;

    /// Declare reads of the simulated sea's textures, which every water set
    /// binds, for a pass that binds one: the draws, and the waterline in the
    /// fog medium. Absent when the graph has no water_fft pass.
    static void declare_simulation_reads(PassBuilder& builder, const DeclareContext& ctx,
                                         gpu::Access access);
    /// The same for a pass that places the surface and does nothing else
    /// with the sea: its displacement alone.
    static void declare_displacement_read(PassBuilder& builder, const DeclareContext& ctx,
                                          gpu::Access access);

    /// Draw every water node's depth, and nothing else, into the water's
    /// depth attachment the seed filled, resolving it into the surface depth
    /// under MSAA. Recorded by WaterDepthPrepass. The colour draw then tests
    /// against it and does not write it, so from here on the surface depth
    /// is final.
    void record_depth(const FrameContext& ctx);

    /// Copy the scene depth into the water's own depth image, so the sea is
    /// still hidden by whatever opaque geometry stands in front of it.
    /// Recorded by WaterDepthSeedPass, which declares both images: the scene
    /// depth as a copy source and the water's as a copy target.
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
        gpu::Owned<gpu::GraphicsPipeline> pipeline;
        uint32_t generation{0};   ///< PipelineCache generation it was built at
    };

    /// Work out every water node's wave field for the frame. Each draw binds
    /// its node's as a copy of its own.
    void gather_fields(const FrameContext& ctx);

    /// Gather every water node's wakes into one list for the frame and note
    /// where each node's begin.
    void gather_wakes(const FrameContext& ctx);
    /// The frame's wakes as the water set binds them: never empty.
    [[nodiscard]] std::span<const GpuWakeSource> wake_records() const;

    /// Which of the water set's bindings a pipeline's shaders declare: the
    /// depth prepass the field, the clipmap, the ground, the sea's
    /// displacement and the wakes; the colour draw the sea's previous frame
    /// and derivatives too, and in its fragment stage the fog's medium; the
    /// waterline the field, the ground, the sea's displacement and the wakes.
    enum class WaterSet : uint8_t { depth, colour, line };
    /// The ground under one body of water as its draws bind it: its
    /// terrain's heights and how open the water is to the sea, either unset
    /// where it has none.
    struct Ground {
        gpu::Texture bed;
        gpu::Texture exposure;
        gpu::Texture warp;
    };
    /// The water set for one draw or dispatch: `field` and `clipmap` and
    /// `wakes` copies made for it, `medium` the fog's volume or unset, and the
    /// ground under this water.
    [[nodiscard]] std::vector<gpu::BindEntry> water_set(WaterSet use, gpu::BufferRange field,
                                                        gpu::BufferRange clipmap, gpu::BufferRange wakes,
                                                        gpu::Texture medium, Ground ground) const;
    /// The ground under `water` this frame.
    [[nodiscard]] Ground ground_of(const WaterComponent* water) const;

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
    void record_water_line(const FrameContext& ctx, gpu::Texture mask);

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
    gpu::Owned<gpu::ComputePipeline> line_pipeline_;

    /// Bind, push and draw every water node with a usable material, with its
    /// material's colour pipeline, or with `depth_only` for all of them when
    /// that is given. One loop for both, so the prepass and the colour draw
    /// always cover the same nodes. `medium` is the fog's integrated volume
    /// when this run built one, for the nodes that name a medium. Inside a
    /// rendering scope.
    void draw_nodes(const FrameContext& ctx, gpu::RenderEncoder& pass, gpu::GraphicsPipeline depth_only,
                    gpu::Texture medium);

    /// Build (or rebuild after a recompile) the colour pipeline for a water
    /// FJSL entry. Returns unset when the entry failed to compile or is not a
    /// water shader, in which case the fallback pipeline draws instead.
    gpu::GraphicsPipeline ensure_material_pipeline(uint32_t pipeline_id);

    /// The colour pipeline and material row a node draws with: its own
    /// material when that is a water shader that compiled, else the engine's
    /// water.fjsl at its defaults.
    void resolve_node_material(const WaterComponent& comp, gpu::GraphicsPipeline& out_pipeline,
                               uint32_t& out_material_id);

    /// Register the engine's water.fjsl and give it a material row, so a node
    /// with no material of its own still draws.
    void ensure_fallback_material();

    gpu::Device* device_{nullptr};
    PassResources resources_{};
    /// Where the scene SDF sits, refreshed each frame through set_sdf_bounds().
    /// Equal min and max mean no SDF, which the shader reads as no geometry
    /// near rather than dividing by a zero extent.
    glm::vec3 sdf_volume_min_{0.0F};
    glm::vec3 sdf_volume_max_{0.0F};
    /// The prepass pipeline: the same task and mesh stages, no fragment stage,
    /// no colour attachments.
    gpu::Owned<gpu::GraphicsPipeline> depth_pipeline_;
    /// The target the prepass drew into this frame, or null. The colour draw
    /// tests against that depth, and does not draw without it.
    const OffscreenPass* depth_drawn_for_{nullptr};
    bool warned_no_prepass_{false};
    /// The simulated sea's textures, which every water draw samples. Owned by
    /// the registry; null only if the render graph lists no water_fft pass.
    WaterFftPass* simulation_{nullptr};
    /// The volumetric fog, whose froxel range the water maps its medium
    /// samples by; null when the graph has no fog.
    FogCore* fog_{nullptr};
    /// Bound in place of the fog's volume where there is none: one texel
    /// of air, which nothing samples while there is no medium.
    gpu::Owned<gpu::Texture> no_medium_;
    /// Bound in place of a terrain's heights where the water has no ground
    /// under it: one texel, which nothing reads while the field says so.
    gpu::Owned<gpu::Texture> no_bed_;
    /// The same for an exposure: one texel of open sea.
    gpu::Owned<gpu::Texture> no_exposure_;
    /// And for a warp: one texel of no offset.
    gpu::Owned<gpu::Texture> no_warp_;
    /// Whether the scene currently holds a second, different simulated sea and
    /// that has been said, so it is said when it starts and not every frame.
    bool warned_second_sea_{false};
    /// Each node's wave field this frame, and the one value of it the push
    /// constants also need.
    std::unordered_map<const WaterComponent*, GpuWaterField> fields_;
    /// Each node's ground this frame: the height texture of the terrain its
    /// field's shore reads, and its exposure. Absent for a node with no shore.
    std::unordered_map<const WaterComponent*, Ground> grounds_;
    /// One of a node's baked shore grids on the GPU and the bake it came
    /// from. A new bake is a new texture, the old one released once the
    /// frames reading it are done, rather than written over under them.
    template <typename Grid>
    struct ShoreTexture {
        std::shared_ptr<const Grid> from;
        gpu::Owned<gpu::Texture> texture;
    };
    std::unordered_map<const WaterComponent*, ShoreTexture<water::ShoreExposure>> exposures_;
    std::unordered_map<const WaterComponent*, ShoreTexture<water::ShoreWarp>> warps_;
    /// The levels an infinite surface is drawn from, recentered on the camera
    /// once per frame. One set of levels for the scene rather than one per
    /// node: they follow the camera, not the water, so two infinite seas at
    /// different heights share them.
    WaterClipmap clipmap_;

    /// How many wake records a frame holds across every water node. Past it
    /// a node's remaining wakes are not drawn that frame.
    static constexpr uint32_t WAKE_RECORDS_PER_FRAME = 64;
    /// The wakes the mesh and fragment stages read, every node's one after
    /// another, and where each node's are.
    std::vector<GpuWakeSource> wakes_;
    struct WakeRange {
        uint32_t first{0};
        uint32_t count{0};
    };
    std::unordered_map<const WaterComponent*, WakeRange> wake_ranges_;

    PipelineCache* pipeline_cache_{nullptr};
    BindlessTextures* bindless_{nullptr};
    std::unordered_map<uint32_t, MaterialPipeline> material_pipelines_;
    /// Entries already reported as unusable, so the log says it once.
    std::unordered_set<uint32_t> reported_entries_;
    uint32_t fallback_pipeline_id_{0};
    /// The fallback's material row, made once; `NO_ROW` until then.
    static constexpr uint32_t NO_ROW = ~0u;
    uint32_t fallback_material_id_{NO_ROW};
    bool fallback_ready_{false};
};

/// Seeds the water's depth attachment from the finished opaque scene, as a
/// graph node of its own so the graph moves both images into the copy's
/// layouts and on to where their next readers want them.
class WaterDepthSeedPass : public RenderPass {
public:
    Result<> create(const PassResources& resources) override;
    void destroy() override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;

private:
    WaterPass* water_{nullptr};
};

/// Draws the water's depth, and only its depth, once the seed has filled the
/// attachment: the surface depth the fog volume and the colour draw read.
class WaterDepthPrepass : public RenderPass {
public:
    Result<> create(const PassResources& resources) override;
    void destroy() override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;

private:
    WaterPass* water_{nullptr};
};

} // namespace fjell
