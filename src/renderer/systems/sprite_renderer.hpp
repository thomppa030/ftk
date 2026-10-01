#pragma once

#include "ftk/gpu/buffer.hpp"
#include "ftk/gpu/owned.hpp"
#include "ftk/gpu/pipeline.hpp"
#include "renderer/pass/render_pass.hpp"
#include "renderer/renderer_constants.hpp"
#include "renderer/systems/render_subsystem.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace fjell {

class ResourceManager;
class Scene;

class SpriteRenderer : public RenderSubsystem {
public:
  void create(const PassResources &resources) override;
  void destroy() override;

  // RenderSubsystem lifecycle
  void update(const RenderSubsystemContext &ctx) override;
  void record_depth(const FrameContext& ctx, ftk::gpu::RenderEncoder& pass) override;
  void record_opaque(const FrameContext& ctx, ftk::gpu::RenderEncoder& pass) override;
  void record_transparent(const FrameContext& ctx, ftk::gpu::RenderEncoder& pass) override;
  [[nodiscard]] int transparent_order() const override { return -50; }

  // Resolve texture paths to bindless indices (called externally by editor)
  void sync_textures(Scene &scene, ResourceManager &resources,
                     const class Project *project = nullptr);

private:
  // Internal implementations
  void update_impl(const Scene &scene, const glm::mat4 &view_projection,
                   const glm::vec3 &camera_position, ftk::gpu::Frame &frame,
                   bool playing);
  // Three pipeline buckets, partitioned by (lit, opaque-vs-transparent).
  // Lit-flat sprites render in the opaque pass with depth write on so they
  // act as proper 3D occluders; lit-billboards stay in transparent (no
  // depth write) because their alpha edges shouldn't punch holes.
  enum class Bucket : uint8_t {
    UnlitTransparent = 0,
    LitTransparent   = 1,  // lit billboard
    LitOpaque        = 2,  // lit flat
    Count            = 3,
  };

  // A run of sprites drawn with one bucket's pipeline.
  struct SpriteRun {
    Bucket bucket{Bucket::UnlitTransparent};
    uint32_t first{0};
    uint32_t count{0};
  };

  void record_runs(const FrameContext &ctx, ftk::gpu::RenderEncoder &pass,
                   std::span<const SpriteRun> runs);
  /// Sets `pipeline` and binds what every sprite pipeline reads.
  void use(const FrameContext &ctx, ftk::gpu::RenderEncoder &pass, ftk::gpu::GraphicsPipeline pipeline);

  struct GpuSprite {
    glm::vec3 position;       // 12
    uint32_t flags;           // 4   (bit 0: flat, bit 1: flip_h, bit 2: lit)
    glm::vec2 size;           // 8
    uint32_t texture_index;   // 4
    float opacity;            // 4
    glm::vec4 tint;           // 16
    glm::vec4 uv_rect;        // 16  {u_min, v_min, u_max, v_max}
    uint32_t normal_index;    // 4   bindless slot for normal map (0 = none)
    float roughness;          // 4
    float metallic;           // 4
    float emission;           // 4
  };
  // Mirrored by `Sprite` in shaders/include/fjell_sprite.glsl (std430),
  // which every sprite vertex shader includes.
  static_assert(sizeof(GpuSprite) == 80);
  static_assert(offsetof(GpuSprite, flags) == 12);
  static_assert(offsetof(GpuSprite, size) == 16);
  static_assert(offsetof(GpuSprite, texture_index) == 24);
  static_assert(offsetof(GpuSprite, opacity) == 28);
  static_assert(offsetof(GpuSprite, tint) == 32);
  static_assert(offsetof(GpuSprite, uv_rect) == 48);
  static_assert(offsetof(GpuSprite, normal_index) == 64);
  static_assert(offsetof(GpuSprite, roughness) == 68);
  static_assert(offsetof(GpuSprite, metallic) == 72);
  static_assert(offsetof(GpuSprite, emission) == 76);

  ftk::gpu::Device *gpu_{nullptr};

  // One pipeline per bucket, the same bindings, a different shader pair and
  // state.
  std::array<ftk::gpu::Owned<ftk::gpu::GraphicsPipeline>, static_cast<size_t>(Bucket::Count)> pipelines_;
  // Depth-only pipeline used in the depth prepass. Draws the same lit-opaque
  // sprite range, but writes only depth (no color attachments). Lets lit-flat
  // sprites contribute to Hi-Z, contact shadows, and SS-GI neighbour reads.
  ftk::gpu::Owned<ftk::gpu::GraphicsPipeline> depth_pipeline_;

  /// The sprites the view being recorded draws, in the order below, in
  /// the frame's memory: written by update() for each view's run.
  std::array<ftk::gpu::BufferRange, ftk::MAX_FRAMES_IN_FLIGHT> sprite_ranges_{};

  // Per frame, into that frame's SSBO, which holds the transparent sprites
  // and then the opaque ones: the transparent ones as runs in layer order,
  // unlit and lit billboards interleaved as they sort, and the opaque range.
  std::array<std::vector<SpriteRun>, ftk::MAX_FRAMES_IN_FLIGHT> transparent_runs_;
  std::array<SpriteRun, ftk::MAX_FRAMES_IN_FLIGHT> opaque_range_{};

  static constexpr uint32_t MAX_SPRITES = 4096;

  // Reusable per-frame scratch (avoid heap allocs every frame)
  struct SortEntry {
    Bucket  bucket;     // which pipeline; opaque sprites sort after the rest
    int32_t sort_layer;
    int32_t sort_order;
    float y_position;   // for top-down depth (lower Y = in front)
    float distance;     // for billboard distance sort
    uint32_t index;
    bool flat;
  };
  std::vector<GpuSprite> sprites_scratch_;
  std::vector<SortEntry> sort_scratch_;
  std::vector<GpuSprite> sorted_scratch_;
};

} // namespace fjell
