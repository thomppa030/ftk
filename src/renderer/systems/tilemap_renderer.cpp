#include "renderer/systems/tilemap_renderer.hpp"
#include "gpu/device.hpp"
#include "gpu/render_encoder.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/resources/texture.hpp"
#include "renderer/components/tilemap_component.hpp"
#include "renderer/frustum.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "core/project.hpp"
#include "scene/scene.hpp"

#include <filesystem>
#include <stdexcept>
#include <vector>

namespace fjell {

void TilemapRenderer::create(Device& /*device*/, const PassResources& resources) {
    gpu_ = resources.device;
    gpu::Device& gpu = *gpu_;

    // The scene's four colour targets: colour, metallic/roughness, normal and
    // motion. Tiles write colour, and lit tiles the surface they are too;
    // nothing writes motion, since a tilemap does not move.
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
        if (!made) throw std::runtime_error("Tilemap: " + made.error());
        return std::move(*made);
    };
    // Unlit tiles are the 2D overlay: blended over the scene, depth tested
    // but not written. Lit tiles are an opaque floor that occludes.
    pipelines_[static_cast<size_t>(Bucket::UnlitTransparent)] = make({
        .vertex = "shaders/tilemap.vert",
        .fragment = "shaders/tilemap.frag",
        .depth = {.test = true, .write = false, .compare = gpu::Compare::less_equal},
        .color = targets(gpu::Blend::alpha, false),
        .name = "tilemap",
    });
    pipelines_[static_cast<size_t>(Bucket::LitOpaque)] = make({
        .vertex = "shaders/tilemap_lit.vert",
        .fragment = "shaders/tilemap_lit.frag",
        .depth = {.test = true, .write = true, .compare = gpu::Compare::less_equal},
        .color = targets(gpu::Blend::none, true),
        .name = "tilemap_lit",
    });
    // The lit tiles into the depth pre-pass, alpha-tested.
    depth_pipeline_ = make({
        .vertex = "shaders/tilemap_lit.vert",
        .fragment = "shaders/tilemap_depth.frag",
        .depth = {.test = true, .write = true, .compare = gpu::Compare::less},
        .name = "tilemap_depth",
    });

    for (auto& buffer : tile_buffers_) {
        auto made = gpu.create(gpu::BufferDesc{
            .size = TILE_BUFFER_SIZE,
            .use = gpu::BufferUse::storage,
            .memory = gpu::Memory::upload,
            .name = "tilemap_tiles",
        });
        if (!made) throw std::runtime_error("Tilemap: " + made.error());
        buffer = std::move(*made);
    }

    FJELL_GFX_DEBUG("Tilemap renderer created");
}

void TilemapRenderer::destroy() {
    for (auto& buffer : tile_buffers_) buffer.reset();
    for (auto& pipeline : pipelines_) pipeline.reset();
    depth_pipeline_.reset();
}

void TilemapRenderer::sync_textures(Scene& scene, ResourceManager& resources, const Project* project) {
    for (auto [obj, tm] : scene.view<TilemapComponent>()) {

        if (!tm->texture_loaded && !tm->tileset_texture.empty()) {
            std::string resolved = base_path::resolve_asset(tm->tileset_texture, project);
            if (std::filesystem::exists(resolved)) {
                auto tex_result = tm->pixel_art
                    ? resources.load_texture_nearest(resolved)
                    : resources.load_texture(resolved);
                if (tex_result) {
                    tm->texture_index = resources.bindless_index(*tex_result);
                    auto* texture = resources.get(*tex_result);
                    if (texture) {
                        tm->tex_width = texture->width();
                        tm->tex_height = texture->height();
                        tm->tileset_columns = tm->tex_width / static_cast<uint32_t>(tm->tile_size.x);
                    }
                    tm->texture_loaded = true;
                    tm->dirty = true;
                }
            }
        }

        if (!tm->normal_loaded && !tm->normal_path.empty()) {
            std::string resolved = base_path::resolve_asset(tm->normal_path, project);
            if (std::filesystem::exists(resolved)) {
                auto nm_result = tm->pixel_art
                    ? resources.load_texture_nearest(resolved)
                    : resources.load_texture(resolved);
                if (nm_result) {
                    tm->normal_index = resources.bindless_index(*nm_result);
                    tm->normal_loaded = true;
                }
            }
        }
    }
}

void TilemapRenderer::update(const RenderSubsystemContext& ctx) {
    if (ctx.scene) {
        update_impl(*ctx.scene, ctx.view_projection, ctx.frame_index);
    }
}

void TilemapRenderer::record_depth(const FrameContext& ctx, gpu::RenderEncoder& pass) {
    FJELL_PROFILE_SCOPE_N("tilemap_record_depth");
    if (tile_buckets_[ctx.frame_index] != Bucket::LitOpaque) return;
    draw(ctx, pass, depth_pipeline_);
}

void TilemapRenderer::record_opaque(const FrameContext& ctx, gpu::RenderEncoder& pass) {
    if (tile_buckets_[ctx.frame_index] == Bucket::LitOpaque) {
        draw(ctx, pass, pipelines_[static_cast<size_t>(Bucket::LitOpaque)]);
    }
}

void TilemapRenderer::record_transparent(const FrameContext& ctx, gpu::RenderEncoder& pass) {
    if (tile_buckets_[ctx.frame_index] == Bucket::UnlitTransparent) {
        draw(ctx, pass, pipelines_[static_cast<size_t>(Bucket::UnlitTransparent)]);
    }
}

void TilemapRenderer::update_impl(const Scene& scene, const glm::mat4& /*view_projection*/,
                                   uint32_t frame_index) {
    FJELL_PROFILE_SCOPE_N("tilemap_update");

    uint32_t total_tiles = 0;
    PushConstants pc{};

    // For now we just use the first tilemap found
    // TODO: support multiple tilemaps
    for (auto [obj, tm] : scene.view<TilemapComponent>()) {
        if (!tm->texture_loaded) continue;
        if (tm->tiles.empty()) continue;

        const auto* spatial = obj->as_spatial();
        if (!spatial) continue;
        glm::vec3 origin = glm::vec3(spatial->world_matrix_[3]);

        pc.texture_index = tm->texture_index;
        pc.z_depth = origin.z;
        pc.normal_index = tm->normal_index;
        pc.flags = tm->lit ? 1u : 0u;
        pc.roughness = tm->roughness;
        pc.metallic = tm->metallic;
        pc.emission = tm->emission;

        tile_buckets_[frame_index] = tm->lit ? Bucket::LitOpaque : Bucket::UnlitTransparent;

        int cols = tm->map_size.x;
        int rows = tm->map_size.y;

        auto* dst = reinterpret_cast<GpuTile*>(gpu_->mapped(tile_buffers_[frame_index]).data());

        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                int idx = r * cols + c;
                if (idx >= static_cast<int>(tm->tiles.size())) break;
                int32_t tile_id = tm->tiles[idx];
                if (tile_id < 0) continue;
                if (total_tiles >= MAX_TILES) break;

                // Tile position in world space
                float x = origin.x + static_cast<float>(c) * tm->world_tile_size;
                float y = origin.y - static_cast<float>(r) * tm->world_tile_size; // row 0 = top

                dst[total_tiles].position = {x, y};
                dst[total_tiles].size = {tm->world_tile_size, tm->world_tile_size};

                // Compute UV from tile_id using tileset grid
                uint32_t tileset_cols = tm->tileset_columns;
                if (tileset_cols == 0) continue;
                uint32_t tc = static_cast<uint32_t>(tile_id) % tileset_cols;
                uint32_t tr = static_cast<uint32_t>(tile_id) / tileset_cols;
                float tw = static_cast<float>(tm->tile_size.x);
                float th = static_cast<float>(tm->tile_size.y);
                float tex_w = static_cast<float>(tm->tex_width);
                float tex_h = static_cast<float>(tm->tex_height);
                float u0 = (static_cast<float>(tc) * tw) / tex_w;
                float v0 = (static_cast<float>(tr) * th) / tex_h;
                float u1 = u0 + tw / tex_w;
                float v1 = v0 + th / tex_h;
                dst[total_tiles].uv_rect = {u0, v0, u1, v1};

                total_tiles++;
            }
        }
        break; // only first tilemap for now
    }

    tile_counts_[frame_index] = total_tiles;
    push_constants_[frame_index] = pc;
}

void TilemapRenderer::draw(const FrameContext& ctx, gpu::RenderEncoder& pass,
                           gpu::GraphicsPipeline pipeline) {
    FJELL_PROFILE_SCOPE_N("tilemap_record_bucket");
    const uint32_t tiles = tile_counts_[ctx.frame_index];
    if (tiles == 0) return;

    pass.set_pipeline(pipeline);
    pass.bind(ctx.globals);
    pass.bind(ctx.bindless);
    pass.bind({{"TileBuffer", gpu::storage(tile_buffers_[ctx.frame_index])}});
    pass.push(push_constants_[ctx.frame_index]);
    pass.draw(tiles * 6);
}

} // namespace fjell
