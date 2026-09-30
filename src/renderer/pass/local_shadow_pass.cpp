#include "renderer/pass/local_shadow_pass.hpp"

#include "core/log.hpp"
#include "core/profiler.hpp"
#include "gpu/device.hpp"
#include "renderer/global_bindings.hpp"
#include "renderer/gpu_context.hpp"
#include "renderer/indirect_batch.hpp"
#include "renderer/pass/shadow_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/render_queue.hpp"
#include "renderer/renderer_constants.hpp"
#include "renderer/resources/mega_buffer.hpp"
#include "renderer/resources/skeletal_model.hpp"
#include "renderer/resources/vertex.hpp"
#include "scene/light_node.hpp"
#include "scene/scene.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace fjell {

// ── Pipeline ────────────────────────────────────────────────────────────

Result<> LocalShadowPass::create(Device& /*device*/, const PassResources& resources) {
    device_ = resources.device;
    written_ = false;
    FJELL_TRY(atlas_, device_->create(gpu::TextureDesc{
                          .format = DEPTH_FORMAT,
                          .width = ATLAS_SIZE,
                          .height = ATLAS_SIZE,
                          .use = gpu::TextureUse::depth_target | gpu::TextureUse::sampled,
                          .name = "local_shadow_atlas",
                      }));
    // The cascades' comparison: filtered, and outside the map reads as lit.
    sampler_ = device_->sampler({.filter = gpu::Filter::linear,
                                 .mip_filter = gpu::Filter::nearest,
                                 .address = gpu::Address::border,
                                 .compare = gpu::Compare::less_equal,
                                 .border = gpu::Border::opaque_white});

    // The cascades' caster state: clamped depth, front faces culled, their
    // bias. Skinned casters cull nothing.
    const gpu::Raster raster{.cull = gpu::Cull::front,
                             .depth_clamp = true,
                             .depth_bias = ShadowPass::DEPTH_BIAS_CONSTANT,
                             .depth_bias_slope = ShadowPass::DEPTH_BIAS_SLOPE};
    const gpu::Depth depth{.test = true, .write = true, .compare = gpu::Compare::less_equal};
    FJELL_TRY(pipeline_, device_->create(gpu::GraphicsPipelineDesc{
                             .vertex = "shaders/shadow.vert",
                             .fragment = "shaders/depth_only.frag",
                             .vertex_layout = Vertex::layout(),
                             .raster = raster,
                             .depth = depth,
                             .depth_format = DEPTH_FORMAT,
                             .shared = {resources.globals, resources.draw_data},
                             .name = "local_shadow",
                         }));
    gpu::Raster skinned_raster = raster;
    skinned_raster.cull = gpu::Cull::none;
    FJELL_TRY(skinned_pipeline_, device_->create(gpu::GraphicsPipelineDesc{
                                     .vertex = "shaders/skinned_shadow.vert",
                                     .fragment = "shaders/depth_only.frag",
                                     .vertex_layout = SkinnedVertex::layout(),
                                     .raster = skinned_raster,
                                     .depth = depth,
                                     .depth_format = DEPTH_FORMAT,
                                     .shared = {resources.globals, resources.draw_data},
                                     .name = "skinned_local_shadow",
                                 }));

    FJELL_GFX_DEBUG("Local shadow atlas created: {}x{}", ATLAS_SIZE, ATLAS_SIZE);
    return {};
}

void LocalShadowPass::post_create(GpuContext& gpu, PassRegistry& /*passes*/) {
    gpu.global_bindings().set("local_shadow_atlas", gpu::sampled(atlas_, sampler_));
}

void LocalShadowPass::destroy() {
    pipeline_.reset();
    skinned_pipeline_.reset();
    atlas_.reset();
}

// ── Per-frame tile assignment ──────────────────────────────────────────

void LocalShadowPass::assign_spot_tiles(const FrameContext& ctx) {
    spot_tiles_.clear();
    if (!active_scene_) return;

    uint32_t tile_x = 0;
    uint32_t tile_y = 0;
    uint32_t tile_size = LOCAL_SHADOW_TILE_SIZE;
    uint32_t tiles_per_row = ATLAS_SIZE / tile_size;

    // Collect shadowed spot lights, sorted by distance to camera
    struct SpotCandidate {
        const SpotLightNode* node;
        uint32_t light_index;
        float distance_sq;
    };
    std::vector<SpotCandidate> candidates;

    // A light's index is its place in its kind's list, the same walk the
    // light SSBO is filled by.
    uint32_t spot_index = 0;
    active_scene_->each_node<SpotLightNode>([&](const SpotLightNode& light) {
        const uint32_t this_index = spot_index++;
        if (!light.cast_shadows) return;

        glm::vec3 pos = glm::vec3(light.world_matrix_[3]);
        glm::vec3 diff = pos - ctx.camera_position;
        candidates.push_back({&light, this_index, glm::dot(diff, diff)});
    });

    // Sort by distance, take closest N
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) { return a.distance_sq < b.distance_sq; });

    uint32_t count = std::min(static_cast<uint32_t>(candidates.size()),
                              MAX_SHADOWED_SPOT_LIGHTS);

    for (uint32_t i = 0; i < count; ++i) {
        const auto& c = candidates[i];
        const auto* sl = c.node;

        glm::vec3 pos = glm::vec3(sl->world_matrix_[3]);
        glm::vec3 dir = glm::normalize(glm::mat3(sl->world_matrix_) * sl->direction);

        // Perspective projection matching the spot cone
        float fov = glm::radians(sl->outer_angle) * 2.0f;
        fov = std::min(fov, glm::radians(170.0f)); // clamp to avoid degenerate projection
        glm::mat4 proj = glm::perspectiveZO(fov, 1.0f, 0.1f, sl->radius);

        // View matrix: look from light position along direction
        glm::vec3 up = (std::abs(dir.y) > 0.99f) ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
        glm::mat4 view = glm::lookAt(pos, pos + dir, up);

        ShadowTile tile{};
        tile.x = tile_x * tile_size;
        tile.y = tile_y * tile_size;
        tile.size = tile_size;
        tile.light_vp = proj * view;
        tile.light_index = c.light_index;
        tile.is_spot = true;
        spot_tiles_.push_back(tile);

        // Advance tile position in atlas grid
        tile_x++;
        if (tile_x >= tiles_per_row) {
            tile_x = 0;
            tile_y++;
        }
    }
}

// ── Point light tile assignment ───────────────────────────────────────

void LocalShadowPass::assign_point_tiles(const FrameContext& ctx) {
    point_blocks_.clear();
    if (!active_scene_) return;

    uint32_t tile_size = LOCAL_SHADOW_TILE_SIZE;

    // Point light blocks go after spot tiles in the atlas.
    // Each point light uses a 2x3 block of tiles (6 faces).
    // Start from the row after the last spot tile row.
    uint32_t spot_rows = !spot_tiles_.empty()
        ? (spot_tiles_.back().y / tile_size) + 1
        : 0;

    // Point blocks are 2 tiles wide, 3 tiles tall
    uint32_t block_w = 2 * tile_size;
    uint32_t block_h = 3 * tile_size;

    uint32_t block_x = 0;
    uint32_t block_y = spot_rows * tile_size;

    // Collect shadowed point lights, sorted by distance to camera
    struct PointCandidate {
        const PointLightNode* node;
        uint32_t light_index;
        float distance_sq;
    };
    std::vector<PointCandidate> candidates;

    uint32_t point_index = 0;
    active_scene_->each_node<PointLightNode>([&](const PointLightNode& light) {
        const uint32_t this_index = point_index++;
        if (!light.cast_shadows) return;

        glm::vec3 pos = glm::vec3(light.world_matrix_[3]);
        glm::vec3 diff = pos - ctx.camera_position;
        candidates.push_back({&light, this_index, glm::dot(diff, diff)});
    });

    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) { return a.distance_sq < b.distance_sq; });

    uint32_t count = std::min(static_cast<uint32_t>(candidates.size()),
                              MAX_SHADOWED_POINT_LIGHTS);

    // Standard cubemap face directions and up vectors
    static const glm::vec3 face_dirs[6] = {
        { 1,  0,  0}, // +X
        {-1,  0,  0}, // -X
        { 0,  1,  0}, // +Y
        { 0, -1,  0}, // -Y
        { 0,  0,  1}, // +Z
        { 0,  0, -1}, // -Z
    };
    static const glm::vec3 face_ups[6] = {
        { 0, -1,  0}, // +X
        { 0, -1,  0}, // -X
        { 0,  0,  1}, // +Y
        { 0,  0, -1}, // -Y
        { 0, -1,  0}, // +Z
        { 0, -1,  0}, // -Z
    };

    constexpr float near = 0.1f;

    for (uint32_t i = 0; i < count; ++i) {
        const auto& c = candidates[i];
        glm::vec3 pos = glm::vec3(c.node->world_matrix_[3]);
        float radius = c.node->radius;

        // Check atlas overflow
        if (block_y + block_h > ATLAS_SIZE) break;

        glm::mat4 proj = glm::perspectiveZO(glm::radians(90.0f), 1.0f, near, radius);

        PointShadowBlock block{};
        block.x = block_x;
        block.y = block_y;
        block.tile_size = tile_size;
        block.light_index = c.light_index;
        block.near_plane = near;
        block.far_plane = radius;

        for (int f = 0; f < 6; ++f) {
            block.face_vp[f] = proj * glm::lookAt(pos, pos + face_dirs[f], face_ups[f]);
        }

        point_blocks_.push_back(block);

        // Advance block position
        block_x += block_w;
        if (block_x + block_w > ATLAS_SIZE) {
            block_x = 0;
            block_y += block_h;
        }
    }
}

void LocalShadowPass::prepare_frame(const FrameContext& ctx, GpuContext& /*gpu*/) {
    active_scene_ = const_cast<Scene*>(ctx.scene);
    assign_spot_tiles(ctx);
    assign_point_tiles(ctx);
}

// ── Setup / build ──────────────────────────────────────────────────────

PassSetup LocalShadowPass::setup() const {
    return {
        .name = "local_shadows",
        .phase = RenderPhase::Environment,
        .sub_order = 110, // after cascade shadows (100)
    };
}

void LocalShadowPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    auto atlas = builder.import_named(ctx, "local_shadow_atlas");
    if (atlas.valid()) {
        if (has_work()) {
            builder.write(atlas, gpu::Access::depth_attachment);
        } else {
            // No light casts a shadow, but the global set still binds the
            // atlas: reading it at rest has the graph keep it where it is
            // sampled, from the first frame on. That is the pass's whole
            // effect, and it has no output to keep it from being culled.
            builder.read(atlas, gpu::Access::sampled_fragment);
            builder.has_side_effects();
        }
    }
    // Draws the camera batch's commands through the vertex path, whose
    // instanced draws read the transforms the instance cull keeps. It runs
    // before the cull, so these reads are what the cull has to wait for
    // before overwriting either buffer.
    builder.read(builder.import_named_buffer(ctx, "instance_transforms"),
                 gpu::Access::storage_buffer_read_vertex);
    builder.read(builder.import_named_buffer(ctx, "batch_indirect"),
                 gpu::Access::indirect_read);
}

void LocalShadowPass::collect_exports(DeclareContext& ctx) {
    if (!atlas_) { return; }
    // The lit surfaces sample it in the fragment shader and the fog light in
    // compute, both through the global set. Nothing is in it before the pass
    // first draws.
    ctx.export_texture("local_shadow_atlas", atlas_,
                       {.persistent = true,
                        .resting = gpu::Access::sampled_fragment | gpu::Access::sampled_compute,
                        .unwritten = !written_});
}

// ── Recording ──────────────────────────────────────────────────────────

void LocalShadowPass::draw_casters(const FrameContext& ctx, gpu::RenderEncoder& pass, uint32_t x,
                                   uint32_t y, uint32_t size, const glm::mat4& light_vp) const {
    pass.set_viewport({.x = static_cast<float>(x),
                       .y = static_cast<float>(y),
                       .width = static_cast<float>(size),
                       .height = static_cast<float>(size)});
    pass.set_scissor({.x = static_cast<int32_t>(x), .y = static_cast<int32_t>(y), .width = size, .height = size});

    // Every static draw of the camera batch in one indirect draw: the ranges
    // follow each other from the first draw, and no material's vertex stage
    // is drawn here.
    const IndirectBatch& batch = *ctx.indirect_batch;
    const uint32_t draws = batch.opaque_draw_count(ctx.frame_index);
    if (draws > 0) {
        pass.set_pipeline(pipeline_);
        pass.bind(ctx.globals);
        pass.bind(ctx.draw_data);
        pass.push(light_vp);
        ctx.mega_buffer->bind(pass);
        pass.draw_indexed_indirect(batch.indirect_commands(ctx.frame_index, 0, draws), draws,
                                   sizeof(gpu::DrawIndexedArgs));
    }

    // Skinned meshes, each from its own vertices.
    const auto& skinned_indices = batch.skinned_draw_indices(ctx.frame_index);
    if (!skinned_indices.empty() && ctx.queue) {
        pass.set_pipeline(skinned_pipeline_);
        pass.bind(ctx.globals);
        pass.bind(ctx.draw_data);
        pass.push(light_vp);
        uint32_t skinned_cmd_idx = 0;
        for (const auto& dc : ctx.queue->commands()) {
            if (!dc.skinned || !dc.skeletal_model) continue;
            if (skinned_cmd_idx >= skinned_indices.size()) break;
            dc.skeletal_model->bind(pass);
            pass.draw_indexed(dc.skeletal_model->index_count(), 1, 0, 0, skinned_indices[skinned_cmd_idx]);
            skinned_cmd_idx++;
        }
    }
}

void LocalShadowPass::record(const FrameContext& ctx) {
    FJELL_PROFILE_SCOPE_N("record_local_shadows");
    if (!has_work() || !atlas_) return;
    if (!ctx.mega_buffer || !ctx.indirect_batch) return;

    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Local Shadows");
    // The whole atlas in one scope, cleared as it opens; each light's tiles
    // are drawn through a viewport and scissor of their own.
    auto pass = cmd.render({.depth = gpu::DepthAttachment{.view = atlas_, .load = gpu::Load::clear}});

    for (const auto& tile : spot_tiles_) {
        draw_casters(ctx, pass, tile.x, tile.y, tile.size, tile.light_vp);
    }

    // Point light faces, six tiles per light in a 2x3 block.
    for (const auto& block : point_blocks_) {
        const uint32_t ts = block.tile_size;
        for (uint32_t face = 0; face < 6; ++face) {
            draw_casters(ctx, pass, block.x + (face % 2) * ts, block.y + (face / 2) * ts, ts,
                         block.face_vp[face]);
        }
    }
    written_ = true;
}

} // namespace fjell
