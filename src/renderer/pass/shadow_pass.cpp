#include "renderer/pass/shadow_pass.hpp"

#include "core/log.hpp"
#include "core/profiler.hpp"
#include "environment/environment_state.hpp"
#include "gpu/device.hpp"
#include "renderer/global_bindings.hpp"
#include "renderer/gpu_context.hpp"
#include "renderer/indirect_batch.hpp"
#include "renderer/pass/terrain_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/pass_registry.hpp"
#include "renderer/render_queue.hpp"
#include "renderer/resources/mega_buffer.hpp"
#include "renderer/resources/skeletal_model.hpp"
#include "renderer/resources/vertex.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace fjell {

// ── Cascade split computation ───────────────────────────────────────────

static std::array<float, ShadowPass::CASCADE_COUNT> compute_cascade_splits(
        float near, float far, float lambda = 0.75f) {
    std::array<float, ShadowPass::CASCADE_COUNT> splits{};
    for (uint32_t i = 0; i < ShadowPass::CASCADE_COUNT; ++i) {
        float p = static_cast<float>(i + 1) / static_cast<float>(ShadowPass::CASCADE_COUNT);
        float log_split = near * std::pow(far / near, p);
        float lin_split = near + (far - near) * p;
        splits[i] = lambda * log_split + (1.0f - lambda) * lin_split;
    }
    return splits;
}

static glm::mat4 compute_light_matrix(const glm::mat4& inv_view_proj,
                                       float split_near, float split_far,
                                       const glm::vec3& light_dir) {
    // Frustum corners in NDC (GLM uses [-1,1] Z range with glm::perspective)
    std::array<glm::vec4, 8> corners = {{
        {-1, -1, -1, 1}, { 1, -1, -1, 1}, {-1,  1, -1, 1}, { 1,  1, -1, 1},
        {-1, -1,  1, 1}, { 1, -1,  1, 1}, {-1,  1,  1, 1}, { 1,  1,  1, 1},
    }};

    // Unproject to world space
    for (auto& c : corners) {
        c = inv_view_proj * c;
        c /= c.w;
    }

    // Interpolate near/far planes to get the cascade sub-frustum
    // NDC depth 0 = near, 1 = far for Vulkan
    std::array<glm::vec3, 8> frustum{};
    for (int i = 0; i < 4; ++i) {
        glm::vec3 near_pt = glm::vec3(corners[i]);
        glm::vec3 far_pt = glm::vec3(corners[i + 4]);
        frustum[i]     = near_pt + split_near * (far_pt - near_pt);
        frustum[i + 4] = near_pt + split_far  * (far_pt - near_pt);
    }

    // Center of the sub-frustum
    glm::vec3 center{0.0f};
    for (const auto& p : frustum) center += p;
    center /= 8.0f;

    // Radius of the sub-frustum (for stable positioning)
    float radius = 0.0f;
    for (const auto& p : frustum) {
        radius = std::max(radius, glm::length(p - center));
    }

    // Light view: look along the light direction
    glm::vec3 light_forward = glm::normalize(light_dir);

    // Pick an up vector that isn't parallel to light direction
    glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
    if (std::abs(glm::dot(light_forward, up)) > 0.99f) {
        up = glm::vec3(1.0f, 0.0f, 0.0f);
    }

    glm::mat4 light_view = glm::lookAt(
        center - light_forward * radius,
        center,
        up);

    // Find AABB of frustum in light space
    float min_x =  std::numeric_limits<float>::max();
    float max_x = -std::numeric_limits<float>::max();
    float min_y =  std::numeric_limits<float>::max();
    float max_y = -std::numeric_limits<float>::max();
    float min_z =  std::numeric_limits<float>::max();
    float max_z = -std::numeric_limits<float>::max();

    for (const auto& p : frustum) {
        auto lp = light_view * glm::vec4(p, 1.0f);
        min_x = std::min(min_x, lp.x);
        max_x = std::max(max_x, lp.x);
        min_y = std::min(min_y, lp.y);
        max_y = std::max(max_y, lp.y);
        min_z = std::min(min_z, lp.z);
        max_z = std::max(max_z, lp.z);
    }

    // Extend Z to catch shadow casters behind the camera frustum
    float z_margin = radius * 2.0f;
    min_z -= z_margin;

    // orthoZO maps [near,far] to [0,1] depth.
    // With lookAt (RH), objects are at negative Z. Pass min_z/max_z directly —
    // glm::orthoZO handles the sign correctly as clip planes.
    glm::mat4 light_proj = glm::orthoZO(min_x, max_x, min_y, max_y, -max_z, -min_z);

    // Snap to texel grid to prevent shadow shimmer when the camera moves.
    // The shadow map origin shifts by sub-texel amounts as the camera orbits,
    // causing shadow edges to crawl. Rounding to texel boundaries fixes this.
    glm::mat4 shadow_matrix = light_proj * light_view;
    float half_map = static_cast<float>(ShadowPass::MAP_SIZE) * 0.5f;
    glm::vec4 origin = shadow_matrix * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    float offset_x = std::round(origin.x * half_map) / half_map - origin.x;
    float offset_y = std::round(origin.y * half_map) / half_map - origin.y;
    shadow_matrix[3][0] += offset_x;
    shadow_matrix[3][1] += offset_y;

    return shadow_matrix;
}

void ShadowPass::prepare_frame(const FrameContext& ctx, GpuContext& /*gpu*/) {
    if (ctx.environment == nullptr) return;

    // Cascades follow the shadow-casting body; the light travels away from it.
    const glm::vec3 light_dir = -ctx.environment->primary.direction;
    update_cascades(ctx.view, ctx.projection, light_dir, 0.1f, 1000.0f);
}

void ShadowPass::update_cascades(const glm::mat4& view, const glm::mat4& projection,
                                  const glm::vec3& light_direction, float near, float far) {
    cascade_splits_ = compute_cascade_splits(near, far);

    glm::mat4 inv_vp = glm::inverse(projection * view);

    float prev_split = 0.0f;
    for (uint32_t i = 0; i < CASCADE_COUNT; ++i) {
        float split = cascade_splits_[i] / far; // normalize to [0,1]
        light_matrices_[i] = compute_light_matrix(
            inv_vp, prev_split, split, light_direction);
        prev_split = split;
    }
}

// ── Pipeline ────────────────────────────────────────────────────────────

Result<> ShadowPass::create(Device& /*device*/, const PassResources& resources) {
    device_ = resources.device;
    written_ = false;
    FJELL_TRY(atlas_, device_->create(gpu::TextureDesc{
                          .format = DEPTH_FORMAT,
                          .width = MAP_SIZE,
                          .height = MAP_SIZE,
                          .layers = CASCADE_COUNT,
                          .use = gpu::TextureUse::depth_target | gpu::TextureUse::sampled,
                          .name = "shadow_map",
                      }));
    // Filtered comparison; outside the map reads as lit.
    sampler_ = device_->sampler({.filter = gpu::Filter::linear,
                                 .mip_filter = gpu::Filter::nearest,
                                 .address = gpu::Address::border,
                                 .compare = gpu::Compare::less_equal,
                                 .border = gpu::Border::opaque_white});

    // Depth clamped rather than clipped, so casters behind the near plane
    // still land in the map; front faces culled and a slope-scaled bias
    // against acne.
    DepthVariantState state{
        .vertex_layout = Vertex::layout(),
        .raster = {.cull = CASTER_CULL,
                   .depth_clamp = true,
                   .depth_bias = DEPTH_BIAS_CONSTANT,
                   .depth_bias_slope = DEPTH_BIAS_SLOPE},
        .depth = {.test = true, .write = true, .compare = gpu::Compare::less_equal},
        .depth_format = DEPTH_FORMAT,
    };
    if (auto made = casters_.create(*device_,
                                    {.vertex = "shaders/shadow.vert", .fragment = "shaders/depth_only.frag"},
                                    state,
                                    {.globals = resources.globals,
                                     .bindless = resources.bindless,
                                     .draw_data = resources.draw_data},
                                    "shadow");
        !made) {
        return made;
    }
    // Skinned casters draw from their own vertices, with the same state.
    state.vertex_layout = SkinnedVertex::layout();
    FJELL_TRY(skinned_pipeline_, device_->create(gpu::GraphicsPipelineDesc{
                                     .vertex = "shaders/skinned_shadow.vert",
                                     .fragment = "shaders/depth_only.frag",
                                     .vertex_layout = state.vertex_layout,
                                     .raster = state.raster,
                                     .depth = state.depth,
                                     .depth_format = DEPTH_FORMAT,
                                     .shared = {resources.globals, resources.draw_data},
                                     .name = "skinned_shadow",
                                 }));

    FJELL_GFX_DEBUG("Shadow map created: {}x{}, {} cascades", MAP_SIZE, MAP_SIZE, CASCADE_COUNT);
    return {};
}

void ShadowPass::post_create(GpuContext& gpu, PassRegistry& /*passes*/) {
    gpu.global_bindings().set("shadow_map", gpu::sampled(atlas_, sampler_));
}

void ShadowPass::destroy() {
    casters_.destroy();
    skinned_pipeline_.reset();
    atlas_.reset();
}

void ShadowPass::record(const FrameContext& ctx) {
    // No early-out on an empty queue: lit surfaces sample every cascade
    // whether or not anything cast into it, so a cascade with nothing to draw
    // is still cleared.
    if (ctx.queue == nullptr || !atlas_) return;

    FJELL_PROFILE_SCOPE_N("record_shadow");
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Cascade Shadows");

    // Terrain casts too, and generates its surface in a mesh shader, so it has
    // no draw in the indirect batches below and has to record itself. A scene
    // whose only caster is the ground — open water with a shoreline is exactly
    // that — has an empty batch in every cascade, so the emptiness test has to
    // account for it or the cascades are cleared and nothing is ever drawn.
    TerrainPass* terrain = ctx.passes != nullptr
                               ? ctx.passes->try_get<TerrainPass>("terrain")
                               : nullptr;
    if (terrain != nullptr && !TerrainPass::scene_has_terrain(ctx)) {
        terrain = nullptr;
    }

    for (uint32_t c = 0; c < CASCADE_COUNT; ++c) {
        // Skinned casters count: they are drawn below the batched meshes and
        // are not among the batch's opaque draws.
        auto* cascade_batch = ctx.shadow_cascade_batches[c];
        const bool has_meshes = cascade_batch != nullptr
                                && (cascade_batch->opaque_draw_count(ctx.frame_index) > 0
                                    || !cascade_batch->skinned_draw_indices(ctx.frame_index).empty())
                                && ctx.shadow_cascade_draw_data_groups[c].valid();

        // The cascade's layer, cleared as its scope opens.
        auto pass = cmd.render({.depth = gpu::DepthAttachment{.view = gpu::layer(atlas_, c),
                                                               .load = gpu::Load::clear}});
        if (has_meshes) {
            const gpu::BindGroup draw_data = ctx.shadow_cascade_draw_data_groups[c];
            auto bind = [&](bool custom) {
                pass.bind(ctx.globals);
                if (custom) pass.bind(ctx.bindless);
                pass.bind(draw_data);
                pass.push(light_matrices_[c]);
            };

            // This cascade's batched casters, each range with its material's
            // pipeline.
            if (ctx.mega_buffer) {
                ctx.mega_buffer->bind(pass);
                casters_.draw_batch(pass, *cascade_batch, ctx.frame_index,
                                    [](uint32_t) { return CASTER_CULL; }, bind);
            }

            // Skinned casters, each from its own vertices.
            const auto& skinned_indices = cascade_batch->skinned_draw_indices(ctx.frame_index);
            if (!skinned_indices.empty() && ctx.shadow_cascade_commands[c]) {
                pass.set_pipeline(skinned_pipeline_);
                bind(false);
                uint32_t idx = 0;
                for (const auto& dc : *ctx.shadow_cascade_commands[c]) {
                    if (!dc.skinned || !dc.skeletal_model) continue;
                    if (idx >= skinned_indices.size()) break;
                    dc.skeletal_model->bind(pass);
                    pass.draw_indexed(dc.skeletal_model->index_count(), 1, 0, 0, skinned_indices[idx]);
                    idx++;
                }
            }
        }

        // Terrain, which generates its surface in a mesh shader and so cannot
        // be part of the batches above. It still records natively into this
        // scope, binding its own pipeline and sets; the cascade index picks
        // the light matrix out of the global UBO.
        if (terrain != nullptr) {
            terrain->record_shadow(ctx, c);
        }
    }
    written_ = true;
}

PassSetup ShadowPass::setup() const {
    return {
        .name = "shadow",
        .phase = RenderPhase::Environment,
        .sub_order = 100,
    };
}

void ShadowPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    builder.parallel_group(1);
    auto atlas = builder.import_named(ctx, "shadow_atlas");
    if (atlas.valid()) {
        builder.write(atlas, gpu::Access::depth_attachment);
    }
}

void ShadowPass::collect_exports(DeclareContext& ctx) {
    if (!atlas_) { return; }
    // Every lit surface samples it in the fragment shader through the global
    // set. Nothing is in it before the pass first draws.
    ctx.export_texture("shadow_atlas", atlas_,
                       {.persistent = true, .resting = gpu::Access::sampled_fragment, .unwritten = !written_});
}

} // namespace fjell
