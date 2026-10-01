#pragma once

#include "ftk/gpu/buffer.hpp"
#include "ftk/gpu/owned.hpp"
#include "ftk/gpu/pipeline.hpp"
#include "renderer/pass/render_pass.hpp"
#include "renderer/renderer_constants.hpp"
#include "renderer/systems/render_subsystem.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>

namespace fjell {

class Project;
class ResourceManager;
class Scene;

class TilemapRenderer : public RenderSubsystem {
public:
    void create(const PassResources& resources) override;
    void destroy() override;

    // RenderSubsystem lifecycle
    void update(const RenderSubsystemContext& ctx) override;
    void record_depth(const FrameContext& ctx, gpu::RenderEncoder& pass) override;
    void record_opaque(const FrameContext& ctx, gpu::RenderEncoder& pass) override;
    void record_transparent(const FrameContext& ctx, gpu::RenderEncoder& pass) override;
    [[nodiscard]] int transparent_order() const override { return -100; }

    // Resolve texture paths to bindless indices (called externally by editor)
    void sync_textures(Scene& scene, ResourceManager& resources, const Project* project = nullptr);

private:
    // Two pipeline buckets. Lit tilemaps render in the opaque pass with depth
    // write on so they act as 3D-occluding floors; unlit stays transparent
    // for the classic 2D-overlay path.
    enum class Bucket : uint8_t {
        UnlitTransparent = 0,
        LitOpaque        = 1,
        Count            = 2,
    };

    void update_impl(const Scene& scene, const glm::mat4& view_projection, uint32_t frame_index);
    void draw(const FrameContext& ctx, gpu::RenderEncoder& pass, gpu::GraphicsPipeline pipeline);

    struct GpuTile {
        glm::vec2 position; // 8
        glm::vec2 size;     // 8
        glm::vec4 uv_rect;  // 16
    };
    static_assert(sizeof(GpuTile) == 32);

    struct PushConstants {
        uint32_t texture_index;
        float z_depth;
        uint32_t normal_index;
        uint32_t flags;       // bit 0: lit
        float roughness;
        float metallic;
        float emission;
        float _pad;
    };

    gpu::Device* gpu_{nullptr};
    std::array<gpu::Owned<gpu::GraphicsPipeline>, static_cast<size_t>(Bucket::Count)> pipelines_;
    gpu::Owned<gpu::GraphicsPipeline> depth_pipeline_;
    /// The frame slot's tiles, written by update() and read by its frame.
    std::array<gpu::Owned<gpu::Buffer>, MAX_FRAMES_IN_FLIGHT> tile_buffers_;

    // Per-frame tile count + which bucket the (single) drawn tilemap fell into.
    // Multi-tilemap support would split this into per-bucket ranges.
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> tile_counts_{};
    std::array<Bucket,   MAX_FRAMES_IN_FLIGHT> tile_buckets_{};
    std::array<PushConstants, MAX_FRAMES_IN_FLIGHT> push_constants_{};

    static constexpr uint32_t MAX_TILES = 16384;
    static constexpr uint64_t TILE_BUFFER_SIZE = MAX_TILES * sizeof(GpuTile);
};

} // namespace fjell
