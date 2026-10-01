#include "renderer/pass/fog_density_field_pass.hpp"

#include "environment/environment_state.hpp"
#include "ftk/base/profiler.hpp"
#include "ftk/framegraph/pass_builder.hpp"
#include "ftk/gpu/device.hpp"
#include "renderer/global_bindings.hpp"
#include "renderer/gpu_context.hpp"

#include <algorithm>
#include <cmath>
#include <span>

namespace fjell {

ftk::Result<> FogDensityFieldPass::create(const PassResources& resources) {
    device_ = resources.device;
    FTK_TRY(pipeline_, device_->create(ftk::gpu::ComputePipelineDesc{
                             .shader = "shaders/fog_density_field.comp",
                             .name = "fog_density_field",
                         }));
    // Outside the box the air is undisturbed, which is what a zero border says.
    sampler_ = device_->sampler({.filter = ftk::gpu::Filter::linear,
                                 .mip_filter = ftk::gpu::Filter::nearest,
                                 .address = ftk::gpu::Address::border,
                                 .border = ftk::gpu::Border::transparent_black});
    // Both fields start undisturbed, so anything reading before the pass has
    // run reads still air.
    for (auto& field : fields_) {
        FTK_TRY(field, device_->create(ftk::gpu::TextureDesc{
                             .kind = ftk::gpu::TextureKind::tex3d,
                             .format = FIELD_FORMAT,
                             .width = RESOLUTION_XZ,
                             .height = RESOLUTION_Y,
                             .depth = RESOLUTION_XZ,
                             .use = ftk::gpu::TextureUse::storage | ftk::gpu::TextureUse::sampled,
                             .initial = ftk::gpu::Clear{0.0f},
                             .name = "fog_density_field",
                         }));
    }
    return {};
}

void FogDensityFieldPass::destroy() {
    pipeline_.reset();
    for (auto& field : fields_) field.reset();
}

void FogDensityFieldPass::post_create(GpuContext& gpu, PassRegistry& /*passes*/) {
    // A field for anything reading before the pass has ever run. Which of the
    // two a reader should see changes every frame, so the binding is rewritten
    // in prepare_frame; this only points the first frame's descriptor at a real
    // image.
    gpu.global_bindings().set("fog_density_field", ftk::gpu::sampled(fields_[cur_], sampler_));
}

void FogDensityFieldPass::prepare_frame(const FrameContext& ctx, GpuContext& gpu) {
    source_count_ = 0;
    moving_count_ = 0;
    if (ctx.environment != nullptr) {
        const auto& sources = ctx.environment->motion_sources;
        source_count_ = std::min(static_cast<uint32_t>(sources.size()), MAX_SOURCES);
        for (uint32_t i = 0; i < source_count_; ++i) {
            const auto& s = sources[i];
            sources_[i] = GpuSource{
                .position_radius = glm::vec4(s.position, s.radius),
                .velocity_reach = glm::vec4(s.velocity, s.height),
                .strength = glm::vec4(s.strength, 0.0f, 0.0f, 0.0f),
            };
            if (glm::length(s.velocity) > STILL_SPEED) { ++moving_count_; }
        }
    }

    // The loudest any volume asks the rolls to be. A wake is the body's, not
    // the volume's, so one number serves the frame.
    vortex_strength_ = 0.0f;
    compression_ = 0.0f;
    if (ctx.environment != nullptr) {
        for (const auto& volume : ctx.environment->fog_volumes) {
            vortex_strength_ = std::max(vortex_strength_, volume.vortex_strength);
            compression_ = std::max(compression_, volume.compression);
        }
    }

    // A box around the camera, snapped to whole texels on every axis. What the
    // field holds then stands still on the ground: with nothing moving, the
    // step back that advection takes lands on a texel centre and reads the
    // value stored there rather than a blend of eight of them. Unsnapped, every
    // texel would be resampled every frame, and a field resampled once a frame
    // dissolves while the camera moves and sharpens when it stops.
    const float texel_xz = EXTENT_METRES / static_cast<float>(RESOLUTION_XZ);
    const float texel_y = HEIGHT_METRES / static_cast<float>(RESOLUTION_Y);
    const glm::vec3 eye = ctx.camera_position;
    const float half = EXTENT_METRES * 0.5f;

    // The box follows the camera across the ground but hangs from whatever is
    // stirring the air, not from the eye. A camera looking down from above —
    // an editor view, a third-person shot — would otherwise carry the box up
    // with it and leave the wake at somebody's feet outside the bottom of it.
    // The lowest source in the scene is what has to be covered; the eye only
    // decides which patch of ground is worth keeping.
    float floor_y = eye.y - FLOOR_BELOW_EYE;
    if (source_count_ > 0) {
        float lowest = sources_[0].position_radius.y;
        for (uint32_t i = 1; i < source_count_; ++i) {
            lowest = std::min(lowest, sources_[i].position_radius.y);
        }
        floor_y = std::min(floor_y, lowest - FLOOR_BELOW_SOURCE);
    }

    const glm::vec3 origin{
        std::floor((eye.x - half) / texel_xz) * texel_xz,
        std::floor(floor_y / texel_y) * texel_y,
        std::floor((eye.z - half) / texel_xz) * texel_xz,
    };

    // The box only moves on a frame that carries the field along with it. A
    // frame that records nothing leaves both where they were: moving the box
    // without a dispatch to resample into it would leave the contents
    // describing one patch of world while everything reading them assumed
    // another, and the field would jump the moment the camera crossed a texel
    // while the pass was idle.
    const bool will_step = moving_count_ > 0 || !settled_;
    if (will_step) {
        prev_origin_ = first_frame_ ? origin : origin_;
        origin_ = origin;
    }

    // World position -> box uvw, for the environment UBO. All three axes carry,
    // unlike the motion field's flat map.
    world_to_uv_ = glm::mat4(0.0f);
    world_to_uv_[0][0] = 1.0f / EXTENT_METRES;
    world_to_uv_[1][1] = 1.0f / HEIGHT_METRES;
    world_to_uv_[2][2] = 1.0f / EXTENT_METRES;
    world_to_uv_[3][0] = -origin_.x / EXTENT_METRES;
    world_to_uv_[3][1] = -origin_.y / HEIGHT_METRES;
    world_to_uv_[3][2] = -origin_.z / EXTENT_METRES;
    world_to_uv_[3][3] = 1.0f;

    // Alive while anything moves, and for as long afterwards as a disturbance
    // takes to settle. A scene nobody has walked through costs nothing.
    active_ = moving_count_ > 0 || !settled_;

    // Which field this frame writes, decided here so the binding can be pointed
    // at it before anything records: a descriptor may not be rewritten once a
    // command buffer referencing it is in flight.
    stepped_this_frame_ = false;
    if (active_) { cur_ = prev_index(); }

    gpu.global_bindings().set(ctx.frame_index, "fog_density_field",
                              ftk::gpu::sampled(fields_[cur_], sampler_));
}

PassSetup FogDensityFieldPass::setup() const {
    return {
        .name = "fog_density_field",
        .phase = RenderPhase::Environment,
        .sub_order = 71,   // beside the motion field; its sources are its own
    };
}

void FogDensityFieldPass::declare(ftk::PassBuilder& builder, const DeclareContext& ctx) {
    // Nothing moves and the air has settled, or an earlier viewport already
    // stepped the field this frame: it is left as it is.
    if ((moving_count_ == 0 && settled_) || stepped_this_frame_) { return; }
    // Last frame's field carried forward into this frame's. The graph moves
    // the one written into GENERAL, and back to rest afterwards.
    builder.read(builder.import_named(ctx, "fog_density_field_prev"),
                 ftk::gpu::Access::sampled_compute);
    builder.write(builder.import_named(ctx, "fog_density_field"),
                  ftk::gpu::Access::storage_write_compute);
}

void FogDensityFieldPass::collect_exports(DeclareContext& ctx) {
    if (!fields_[cur_]) { return; }
    // Both fields rest where the fog samples them, in compute through the
    // global set, and are carried into the next frame's field.
    const ftk::TextureExport how{.persistent = true, .resting = ftk::gpu::Access::sampled_compute};
    // The field this frame writes is the one the global set binds.
    ctx.export_texture("fog_density_field", fields_[cur_], how);
    ctx.export_texture("fog_density_field_prev", fields_[prev_index()], how);
}

void FogDensityFieldPass::record(const FrameContext& ctx) {
    if (moving_count_ == 0 && settled_) return;
    if (stepped_this_frame_) return;
    stepped_this_frame_ = true;

    FTK_PROFILE_SCOPE_N("record_fog_density_field");
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Fog Density Field");

    const uint32_t write = cur_;
    const uint32_t read = 1 - cur_;

    const float texel_xz = EXTENT_METRES / static_cast<float>(RESOLUTION_XZ);
    const float texel_y = HEIGHT_METRES / static_cast<float>(RESOLUTION_Y);

    // The sustained wind and not the gust: a gust breathes about once a second
    // and stirred air has more mass than that, the same reading fog volumes
    // take of it.
    glm::vec3 wind{0.0f};
    if (ctx.environment != nullptr) {
        const auto& weather = ctx.environment->weather;
        if (glm::length(weather.wind_direction) > 1e-4f) {
            wind = glm::normalize(weather.wind_direction) * weather.wind_speed;
        }
    }

    // A storage buffer holds at least one source, even in a frame with none.
    const auto sources = cmd.transient(std::span(sources_.data(), std::max(source_count_, 1U)));
    cmd.set_pipeline(pipeline_);
    cmd.bind({{"SourceBuffer", ftk::gpu::storage(sources)},
              {"density_prev", ftk::gpu::sampled(fields_[read], sampler_)},
              {"density_out", ftk::gpu::storage(fields_[write])}});
    cmd.push(PushConstants{
        .origin_texel = glm::vec4(origin_, texel_xz),
        .prev_origin_texel_y = glm::vec4(prev_origin_, texel_y),
        .extent_decay_dt = glm::vec4(EXTENT_METRES, HEIGHT_METRES, 1.0f / MEMORY_SECONDS,
                                     ctx.delta_time),
        .wind_fraction = glm::vec4(wind, WIND_FRACTION),
        .res_count = glm::uvec4(RESOLUTION_XZ, RESOLUTION_Y, source_count_, 0),
        .tuning = glm::vec4(SPREAD_PER_SECOND, WAKE_WIDTH, SWIRL_METRES_PER_SECOND,
                            CARVE_PER_SECOND),
        .time = elapsed_,
        .buoyancy = BUOYANCY_METRES_PER_SECOND,
        .vortex_strength = vortex_strength_,
        .compression = compression_,
    });
    const uint32_t groups_xz = (RESOLUTION_XZ + 3) / 4;
    const uint32_t groups_y = (RESOLUTION_Y + 3) / 4;
    cmd.dispatch(groups_xz, groups_y, groups_xz);
    first_frame_ = false;
    elapsed_ += ctx.delta_time;
    // Settled once nothing has moved for as long as a disturbance lasts: by
    // then the last of it has decayed and further frames would write the same
    // empty field again.
    still_seconds_ = moving_count_ > 0 ? 0.0f : still_seconds_ + ctx.delta_time;
    settled_ = still_seconds_ >= MEMORY_SECONDS;
}

} // namespace fjell
