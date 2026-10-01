#include "renderer/pass/cloud_shadow_pass.hpp"

#include "environment/environment_state.hpp"
#include "ftk/base/log.hpp"
#include "ftk/base/profiler.hpp"
#include "ftk/framegraph/pass_builder.hpp"
#include "ftk/gpu/device.hpp"
#include "renderer/global_bindings.hpp"
#include "renderer/gpu_context.hpp"

namespace fjell {

namespace {

struct CloudShadowPush {
    glm::vec2 min_xz;
    float size;
    float texture_size;
    float ground_y;
    float time;
    glm::vec2 pad;
};

} // namespace

Result<> CloudShadowPass::create(const PassResources& resources) {
    gpu::Device& device = *resources.device;
    // Linear sampling across the map: a texel is two metres of ground and
    // the shadow edge should not show it.
    sampler_ = device.sampler({.filter = gpu::Filter::linear, .address = gpu::Address::clamp});
    FJELL_TRY(pipeline_, device.create(gpu::ComputePipelineDesc{
                             .shader = "shaders/cloud_shadow.comp",
                             .shared = {resources.globals},
                             .name = "cloud_shadow",
                         }));
    // Full transmittance until the first dispatch: graphs that never run
    // this pass still bind it.
    FJELL_TRY(map_, device.create(gpu::TextureDesc{
                        .format = gpu::Format::r8_unorm,
                        .width = RESOLUTION,
                        .height = RESOLUTION,
                        .use = gpu::TextureUse::storage | gpu::TextureUse::sampled,
                        .initial = gpu::Clear{1.0f},
                        .name = "cloud_shadow",
                    }));
    cleared_ = true;
    FJELL_GFX_DEBUG("Cloud shadow pass created");
    return {};
}

void CloudShadowPass::post_create(GpuContext& gpu, PassRegistry& /*passes*/) {
    gpu.global_bindings().set("cloud_shadow", gpu::sampled(map_, sampler_));
}

void CloudShadowPass::destroy() {
    pipeline_.reset();
    map_.reset();
}

void CloudShadowPass::prepare_frame(const FrameContext& ctx, GpuContext& /*gpu*/) {
    active_ = false;
    if (ctx.environment == nullptr) return;
    const auto& env = *ctx.environment;
    // Above the grazing band the compute shader fades the shadow out on its
    // own; below it there is nothing to draw, so the map holds full
    // transmittance and the dispatch is skipped entirely.
    active_ = env.weather.cloud_coverage > 0.0f && env.primary.lit() &&
              env.primary.direction.y > 0.05f;
    frame_ = cloud_shadow_frame(ctx.camera_position, EXTENT_METRES, RESOLUTION);
    // The ground the sun's rays are traced from: the camera's height is the
    // one height the map knows about the terrain under it.
    ground_y_ = ctx.camera_position.y - 2.0f;
    time_ += ctx.delta_time;
}

PassSetup CloudShadowPass::setup() const {
    return {
        .name = "cloud_shadow",
        .phase = RenderPhase::Environment,
        .sub_order = 60,   // after the sky cubemap, before anything shades
    };
}

void CloudShadowPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    // Nothing to do while the map already holds full transmittance.
    if (!active_ && cleared_) { return; }
    auto map = builder.import_named(ctx, "cloud_shadow");
    builder.write(map, active_ ? gpu::Access::storage_write_compute : gpu::Access::clear);
}

void CloudShadowPass::collect_exports(DeclareContext& ctx) {
    if (!map_) { return; }
    // Every surface, the fog light and the sun shafts sample it through the
    // global set, which the graph cannot see.
    ctx.export_texture("cloud_shadow", map_,
                       {.resting = gpu::Access::sampled_fragment | gpu::Access::sampled_compute});
}

void CloudShadowPass::record(const FrameContext& ctx) {
    FJELL_PROFILE_SCOPE_N("record_cloud_shadow");
    auto& cmd = ctx.commands();

    if (!active_) {
        // No clouds or no sun: hold full transmittance. Once cleared the map
        // stays cleared until clouds return, so this costs nothing per frame.
        if (cleared_) return;
        cmd.clear(map_, gpu::Clear{1.0f});
        cleared_ = true;
        return;
    }

    auto zone = cmd.zone("Cloud Shadow");
    cmd.set_pipeline(pipeline_);
    cmd.bind(ctx.globals);
    cmd.bind({{"shadow_out", gpu::storage(map_)}});
    cmd.push(CloudShadowPush{
        .min_xz = frame_.min_xz,
        .size = frame_.size,
        .texture_size = static_cast<float>(RESOLUTION),
        .ground_y = ground_y_,
        .time = time_,
        .pad = {},
    });
    const uint32_t groups = (RESOLUTION + 7) / 8;
    cmd.dispatch(groups, groups, 1);
    cleared_ = false;
}

} // namespace fjell
