#pragma once

#include "ftk/gpu/owned.hpp"
#include "ftk/gpu/pipeline.hpp"
#include "ftk/gpu/render_encoder.hpp"
#include "ftk/gpu/sampler.hpp"
#include "ftk/gpu/texture.hpp"
#include "renderer/pass/render_pass.hpp"
#include "renderer/renderer_constants.hpp"
#include "renderer/uniform_types.hpp"

#include <glm/glm.hpp>

#include <array>
#include <vector>

namespace fjell {

class Scene;

// Atlas tile assignment for a single shadow-casting light.
struct ShadowTile {
    uint32_t x{0};          // pixel offset in atlas
    uint32_t y{0};
    uint32_t size{LOCAL_SHADOW_TILE_SIZE};
    glm::mat4 light_vp{1.0f};
    uint32_t light_index{0}; // index into the GPU light SSBO
    bool is_spot{true};
};

// Renders shadow maps for point and spot lights into a 2D depth atlas.
class LocalShadowPass : public RenderPass {
public:
    static constexpr uint32_t ATLAS_SIZE = LOCAL_SHADOW_ATLAS_SIZE;
    static constexpr gpu::Format DEPTH_FORMAT = gpu::Format::d32_float;

    Result<> create(const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void collect_exports(DeclareContext& ctx) override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;
    void prepare_frame(const FrameContext& ctx, GpuContext& gpu) override;

    // Spot light tiles assigned this frame (read by update_light_ssbo to fill shadow fields)
    [[nodiscard]] const std::vector<ShadowTile>& spot_tiles() const { return spot_tiles_; }

    // Point light tile blocks assigned this frame.
    // Each entry represents a 2x3 block of 6 face tiles for one point light.
    // Face layout within the block: faces 0-5 at (col % 2, col / 2) offsets.
    struct PointShadowBlock {
        uint32_t x{0};          // pixel offset of block origin in atlas
        uint32_t y{0};
        uint32_t tile_size{LOCAL_SHADOW_TILE_SIZE};
        uint32_t light_index{0};
        std::array<glm::mat4, 6> face_vp{}; // view-projection per cubemap face
        float near_plane{0.1F};
        float far_plane{1.0F};  // = light radius
    };
    [[nodiscard]] const std::vector<PointShadowBlock>& point_blocks() const { return point_blocks_; }

private:
    void assign_spot_tiles(const FrameContext& ctx);
    void assign_point_tiles(const FrameContext& ctx);
    /// Draws the camera batch's casters into one tile of the atlas, seen
    /// through `light_vp`.
    void draw_casters(const FrameContext& ctx, gpu::RenderEncoder& pass, uint32_t x, uint32_t y,
                      uint32_t size, const glm::mat4& light_vp) const;

    gpu::Device* device_{nullptr};
    gpu::Owned<gpu::Texture> atlas_;
    gpu::Sampler sampler_;

    // Pipelines (the cascades' shadow.vert / depth_only.frag)
    gpu::Owned<gpu::GraphicsPipeline> pipeline_;
    gpu::Owned<gpu::GraphicsPipeline> skinned_pipeline_;

    // Per-frame tile assignments
    std::vector<ShadowTile> spot_tiles_;
    std::vector<PointShadowBlock> point_blocks_;
    /// Whether the pass has drawn into the atlas since it was made.
    bool written_{false};

    /// Whether any light casts a shadow this frame.
    [[nodiscard]] bool has_work() const { return !spot_tiles_.empty() || !point_blocks_.empty(); }

    // Cached scene pointer from prepare_frame
    Scene* active_scene_{nullptr};
};

} // namespace fjell
