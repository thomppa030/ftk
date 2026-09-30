#include "renderer/systems/sprite_renderer.hpp"
#include "gpu/device.hpp"
#include "gpu/frame.hpp"
#include "gpu/render_encoder.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/resources/texture.hpp"
#include "renderer/components/sprite_desc.hpp"
#include "renderer/frustum.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "core/project.hpp"
#include "scene/scene.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace fjell {

void SpriteRenderer::create(const PassResources& resources) {
    gpu_ = resources.device;
    gpu::Device& gpu = *gpu_;

    // The scene's four colour targets: colour, metallic/roughness, normal and
    // motion. Lit sprites write the surface beside the colour; unlit ones
    // leave it alone, and nothing writes motion.
    auto targets = [&](gpu::Blend colour_blend, bool surface) {
        const gpu::ChannelMask surface_channels =
            surface ? gpu::ChannelMask{gpu::Channel::r | gpu::Channel::g} : gpu::ChannelMask{};
        return std::vector<gpu::ColorTarget>{
            {resources.scene_color_format, colour_blend},
            {resources.scene_mr_format, gpu::Blend::none, surface_channels},
            {resources.scene_normal_format, gpu::Blend::none, surface_channels},
            {resources.scene_velocity_format, gpu::Blend::none, gpu::ChannelMask{}},
        };
    };
    auto make = [&](gpu::GraphicsPipelineDesc desc) {
        desc.depth_format = resources.scene_depth_format;
        desc.samples = resources.samples;
        desc.shared = {resources.globals, resources.bindless};
        auto made = gpu.create(desc);
        if (!made) throw std::runtime_error("Sprites: " + made.error());
        return std::move(*made);
    };
    // Transparent sprites blend over the scene, depth tested but not
    // written; lit-flat ones are opaque and occlude.
    pipelines_[static_cast<size_t>(Bucket::UnlitTransparent)] = make({
        .vertex = "shaders/sprite.vert",
        .fragment = "shaders/sprite.frag",
        .depth = {.test = true, .write = false, .compare = gpu::Compare::less_equal},
        .color = targets(gpu::Blend::alpha, false),
        .name = "sprite",
    });
    pipelines_[static_cast<size_t>(Bucket::LitTransparent)] = make({
        .vertex = "shaders/sprite_lit.vert",
        .fragment = "shaders/sprite_lit.frag",
        .depth = {.test = true, .write = false, .compare = gpu::Compare::less_equal},
        .color = targets(gpu::Blend::alpha, true),
        .name = "sprite_lit_billboard",
    });
    pipelines_[static_cast<size_t>(Bucket::LitOpaque)] = make({
        .vertex = "shaders/sprite_lit.vert",
        .fragment = "shaders/sprite_lit.frag",
        .depth = {.test = true, .write = true, .compare = gpu::Compare::less_equal},
        .color = targets(gpu::Blend::none, true),
        .name = "sprite_lit_flat",
    });
    // The lit vertex shader with a tiny alpha-discard fragment, depth only.
    depth_pipeline_ = make({
        .vertex = "shaders/sprite_lit.vert",
        .fragment = "shaders/sprite_depth.frag",
        .depth = {.test = true, .write = true, .compare = gpu::Compare::less},
        .name = "sprite_depth",
    });

    FJELL_GFX_DEBUG("Sprite renderer created");
}

void SpriteRenderer::destroy() {
    for (auto& pipeline : pipelines_) pipeline.reset();
    depth_pipeline_.reset();
}

void SpriteRenderer::sync_textures(Scene& scene, ResourceManager& resources, const Project* project) {
    FJELL_PROFILE_SCOPE_N("sprite_sync_textures");
    for (auto [obj, sprite_ptr] : scene.view<SpriteComponent>()) {

        auto& sprite = *sprite_ptr;
        if (!sprite.texture_loaded && !sprite.texture_path.empty()) {
            std::string resolved = base_path::resolve_asset(sprite.texture_path, project);
            if (std::filesystem::exists(resolved)) {
                auto tex_result = sprite.pixel_art
                    ? resources.load_texture_nearest(resolved)
                    : resources.load_texture(resolved);
                if (tex_result) {
                    sprite.texture_index = resources.bindless_index(*tex_result);
                    auto* texture = resources.get(*tex_result);
                    if (texture) {
                        sprite.tex_width = texture->width();
                        sprite.tex_height = texture->height();
                    }
                    sprite.texture_loaded = true;
                }
            }
        }

        if (!sprite.normal_loaded && !sprite.normal_path.empty()) {
            std::string resolved = base_path::resolve_asset(sprite.normal_path, project);
            if (std::filesystem::exists(resolved)) {
                auto nm_result = resources.load_texture(resolved);
                if (nm_result) {
                    sprite.normal_index = resources.bindless_index(*nm_result);
                    sprite.normal_loaded = true;
                }
            }
        }
    }
}

void SpriteRenderer::update(const RenderSubsystemContext& ctx) {
    if (ctx.scene != nullptr && ctx.frame != nullptr) {
        update_impl(*ctx.scene, ctx.view_projection, ctx.camera_position, *ctx.frame, ctx.playing);
    }
}

void SpriteRenderer::use(const FrameContext& ctx, gpu::RenderEncoder& pass,
                         gpu::GraphicsPipeline pipeline) {
    pass.set_pipeline(pipeline);
    pass.bind(ctx.globals);
    pass.bind(ctx.bindless);
    pass.bind({{"SpriteBuffer", gpu::storage(sprite_ranges_[ctx.frame_index])}});
}

void SpriteRenderer::record_depth(const FrameContext& ctx, gpu::RenderEncoder& pass) {
    FJELL_PROFILE_SCOPE_N("sprite_record_depth");
    const SpriteRun& range = opaque_range_[ctx.frame_index];
    if (range.count == 0) return;
    use(ctx, pass, depth_pipeline_);
    pass.draw(range.count * 6, 1, range.first * 6, 0);
}

void SpriteRenderer::record_opaque(const FrameContext& ctx, gpu::RenderEncoder& pass) {
    record_runs(ctx, pass, std::span(&opaque_range_[ctx.frame_index], 1));
}

void SpriteRenderer::record_transparent(const FrameContext& ctx, gpu::RenderEncoder& pass) {
    // Unlit sprites and lit billboards blend in one list in layer order,
    // switching pipeline where one kind follows the other.
    record_runs(ctx, pass, transparent_runs_[ctx.frame_index]);
}

void SpriteRenderer::update_impl(const Scene& scene, const glm::mat4& view_projection,
                                  const glm::vec3& camera_position, gpu::Frame& frame,
                                  bool playing) {
    FJELL_PROFILE_SCOPE_N("sprite_update");
    const uint32_t frame_index = frame.slot();
    Frustum frustum = Frustum::from_vp_matrix(view_projection);

    // Gather visible sprites (reuse persistent scratch vectors)
    auto& sprites = sprites_scratch_;
    auto& sort_entries = sort_scratch_;
    sprites.clear();
    sort_entries.clear();

    for (auto [obj, sp] : scene.view<SpriteComponent>()) {
        if (!sp->texture_loaded) continue;
        if (playing && sp->editor_only) continue;

        const auto& sprite = *sp;
        const auto* spatial = obj->as_spatial();
        if (!spatial) continue;
        glm::vec3 pos = glm::vec3(spatial->world_matrix_[3]);
        glm::vec3 scale = spatial->transform.scale;

        glm::vec2 half_size;
        if (sprite.pixels_per_unit > 0.0f && sprite.tex_width > 0 && sprite.tex_height > 0) {
            // Auto-size from texture dimensions and UV rect sub-region
            float region_w = (sprite.uv_rect.z - sprite.uv_rect.x) * static_cast<float>(sprite.tex_width);
            float region_h = (sprite.uv_rect.w - sprite.uv_rect.y) * static_cast<float>(sprite.tex_height);
            float w = region_w / sprite.pixels_per_unit;
            float h = region_h / sprite.pixels_per_unit;
            half_size = {w * scale.x * 0.5f, h * scale.y * 0.5f};
        } else {
            half_size = {scale.x * 0.5f, scale.y * 0.5f};
        }

        // Frustum cull with a sphere approximation
        float radius = glm::length(glm::vec3(half_size, 0.0f));
        AABB aabb;
        aabb.min = pos - glm::vec3(radius);
        aabb.max = pos + glm::vec3(radius);
        if (!frustum.intersects(aabb)) continue;

        GpuSprite gs{};
        gs.position = pos;
        gs.flags = 0;
        if (sprite.flat)   gs.flags |= 1u;
        if (sprite.flip_h) gs.flags |= 2u;
        if (sprite.lit)    gs.flags |= 4u;
        gs.size = half_size;
        gs.texture_index = sprite.texture_index;
        gs.opacity = sprite.opacity;
        gs.tint = sprite.tint;
        gs.uv_rect = sprite.uv_rect;
        gs.normal_index = sprite.normal_index;
        gs.roughness = sprite.roughness;
        gs.metallic = sprite.metallic;
        gs.emission = sprite.emission;

        uint32_t idx = static_cast<uint32_t>(sprites.size());
        sprites.push_back(gs);

        Bucket bucket;
        if (!sprite.lit)        bucket = Bucket::UnlitTransparent;
        else if (sprite.flat)   bucket = Bucket::LitOpaque;
        else                    bucket = Bucket::LitTransparent;

        glm::vec3 diff = camera_position - pos;
        float dist_sq = glm::dot(diff, diff);
        sort_entries.push_back({bucket, sprite.sort_layer, sprite.sort_order,
                                pos.y, dist_sq, idx, sprite.flat});
    }

    // Opaque sprites after the transparent ones, which sort by layer and
    // order together, unlit and lit billboards alike, so they blend in the
    // order the layers ask. Within one layer and order, flat sprites come
    // first (higher y behind), then camera-facing ones, farthest first: flat
    // sprites are the plane the others stand on, and height and distance do
    // not compare with each other.
    std::sort(sort_entries.begin(), sort_entries.end(),
              [](const SortEntry& a, const SortEntry& b) {
                  const bool a_opaque = a.bucket == Bucket::LitOpaque;
                  const bool b_opaque = b.bucket == Bucket::LitOpaque;
                  if (a_opaque != b_opaque) return b_opaque;
                  if (a.sort_layer != b.sort_layer) return a.sort_layer < b.sort_layer;
                  if (a.sort_order != b.sort_order) return a.sort_order < b.sort_order;
                  if (a.flat != b.flat) return a.flat;
                  if (a.flat) return a.y_position > b.y_position;
                  return a.distance > b.distance;
              });

    uint32_t count = std::min(static_cast<uint32_t>(sort_entries.size()), MAX_SPRITES);

    auto& runs = transparent_runs_[frame_index];
    auto& opaque = opaque_range_[frame_index];
    runs.clear();
    opaque = {Bucket::LitOpaque, 0, 0};

    // Nothing binds the range without a sprite to draw.
    sprite_ranges_[frame_index] = {};
    if (count > 0) {
        sorted_scratch_.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            sorted_scratch_[i] = sprites[sort_entries[i].index];
        }
        sprite_ranges_[frame_index] = frame.transient(sorted_scratch_);
    }
    for (uint32_t i = 0; i < count; ++i) {
        const Bucket bucket = sort_entries[i].bucket;
        if (bucket == Bucket::LitOpaque) {
            if (opaque.count == 0) opaque.first = i;
            ++opaque.count;
        } else if (!runs.empty() && runs.back().bucket == bucket) {
            ++runs.back().count;
        } else {
            runs.push_back({bucket, i, 1});
        }
    }
}

void SpriteRenderer::record_runs(const FrameContext& ctx, gpu::RenderEncoder& pass,
                                 std::span<const SpriteRun> runs) {
    FJELL_PROFILE_SCOPE_N("sprite_record_runs");
    gpu::GraphicsPipeline bound{};
    for (const SpriteRun& run : runs) {
        if (run.count == 0) continue;
        const gpu::GraphicsPipeline wanted = pipelines_[static_cast<size_t>(run.bucket)];
        if (wanted != bound) {
            use(ctx, pass, wanted);
            bound = wanted;
        }
        // Each sprite is 6 vertices. The vertex shader keys off
        // gl_VertexIndex / 6 to look up the sprite in the buffer; the first
        // vertex shifts that onto the run without reuploading anything.
        pass.draw(run.count * 6, 1, run.first * 6, 0);
    }
}

} // namespace fjell
