#include "renderer/pass/motion_field_pass.hpp"

#include "core/log.hpp"
#include "core/profiler.hpp"
#include "environment/environment_state.hpp"
#include "gpu/device.hpp"
#include "renderer/global_bindings.hpp"
#include "renderer/gpu_context.hpp"
#include "renderer/pass_builder.hpp"

#include <algorithm>
#include <cmath>
#include <span>

namespace fjell {

Result<> MotionFieldPass::create(Device& /*device*/, const PassResources& resources) {
    device_ = resources.device;
    FJELL_TRY(pipeline_, device_->create(gpu::ComputePipelineDesc{
                             .shader = "shaders/motion_field.comp",
                             .name = "motion_field",
                         }));
    // Outside the map nothing has moved, which is what a zero border says.
    sampler_ = device_->sampler({.filter = gpu::Filter::linear,
                                 .address = gpu::Address::border,
                                 .border = gpu::Border::transparent_black});
    // Both maps start still, so anything reading before the pass has run
    // reads that nothing has moved.
    for (auto& map : maps_) {
        FJELL_TRY(map, device_->create(gpu::TextureDesc{
                           .format = FIELD_FORMAT,
                           .width = RESOLUTION,
                           .height = RESOLUTION,
                           .use = gpu::TextureUse::storage | gpu::TextureUse::sampled,
                           .initial = gpu::Clear{0.0f, 0.0f, 0.0f, 0.0f},
                           .name = "motion_field",
                       }));
    }
    return {};
}

void MotionFieldPass::destroy() {
    pipeline_.reset();
    for (auto& map : maps_) map.reset();
}

void MotionFieldPass::post_create(GpuContext& gpu, PassRegistry& /*passes*/) {
    // A first map for anything that reads before the pass has ever run. Which
    // of the two a reader should see changes every frame, so the binding is
    // rewritten in prepare_frame(); this only makes the very first frame's
    // descriptor point at a real image rather than nothing.
    gpu.global_bindings().set("motion_field", gpu::sampled(maps_[cur_], sampler_));
}

void MotionFieldPass::prepare_frame(const FrameContext& ctx, GpuContext& gpu) {
    source_count_ = 0;
    moving_count_ = 0;
    if (ctx.environment != nullptr) {
        const auto& sources = ctx.environment->motion_sources;
        source_count_ = std::min(static_cast<uint32_t>(sources.size()), MAX_SOURCES);
        for (uint32_t i = 0; i < source_count_; ++i) {
            const auto& s = sources[i];
            sources_[i] = GpuSource{
                .position_radius = glm::vec4(s.position, s.radius),
                // World Y of the top of what moved, not its height above
                // itself: a consumer asking "is this froxel above what came
                // through" needs somewhere to measure from.
                .velocity_height = glm::vec4(s.velocity, s.position.y + s.height),
                .strength = glm::vec4(s.strength, 0.0f, 0.0f, 0.0f),
            };
            // A source standing still keeps its own ground marked but stirs
            // nothing new, which is the difference between the map needing
            // another frame of work and having settled.
            if (glm::length(s.velocity) > STILL_SPEED) ++moving_count_;
        }
    }

    // A square around the camera, snapped to whole texels so what the map
    // holds stands still on the ground as the camera moves. Re-centring every
    // frame would resample the whole map and smear it away.
    const float texel = EXTENT_METRES / static_cast<float>(RESOLUTION);
    const glm::vec3 eye = ctx.camera_position;
    const float half = EXTENT_METRES * 0.5f;
    const glm::vec2 origin{std::floor((eye.x - half) / texel) * texel,
                           std::floor((eye.z - half) / texel) * texel};
    floor_y_ = eye.y - half;

    prev_origin_ = first_frame_ ? origin : origin_;
    origin_ = origin;

    // World XZ → map uv, for the environment UBO. Consumers use it rather
    // than rebuilding the projection and drifting from it.
    // World XZ -> map uv. Zero, not identity: an identity's [1][1] would
    // carry world Y into v and send every froxel above a metre outside the
    // map, where the lookup reads as nothing having moved.
    world_to_uv_ = glm::mat4(0.0f);
    world_to_uv_[0][0] = 1.0f / EXTENT_METRES;   // x -> u
    world_to_uv_[2][1] = 1.0f / EXTENT_METRES;   // z -> v
    world_to_uv_[3][0] = -origin.x / EXTENT_METRES;
    world_to_uv_[3][1] = -origin.y / EXTENT_METRES;
    world_to_uv_[3][3] = 1.0f;

    // Alive while anything moves, and for as long afterwards as the map takes
    // to fade. A scene where everyone has stood still for a while costs
    // nothing: the marks under their feet have faded and nothing replaces them.
    active_ = moving_count_ > 0 || !settled_;

    // Which of the two maps this frame writes is decided here rather than
    // while recording, because the map a reader sees is bound now and a
    // descriptor may not be rewritten once a command buffer referencing it is
    // in flight. The pass alternates, so without this the binding would keep
    // pointing at whichever map it was given at startup: right on the frames
    // that happen to land on it, and a read of the map being written on the
    // rest.
    if (active_) cur_ = prev_index();

    // Readers are pointed at the map this frame writes, not the last one.
    // Everything that reads it runs later in the same command buffer, behind
    // the barrier record() ends with, so what they sample is this frame's
    // finished map rather than a frame-old one.
    gpu.global_bindings().set(ctx.frame_index, "motion_field", gpu::sampled(maps_[cur_], sampler_));
}

PassSetup MotionFieldPass::setup() const {
    return {
        .name = "motion_field",
        .phase = RenderPhase::Environment,
        .sub_order = 70,   // after the cloud shadow, before anything shades
    };
}

// Where the map is read: the surfaces and the editor's debug view in the
// fragment shader, the fog in compute. A reader at any other stage declares
// its read and gets its own barrier from the graph.
constexpr gpu::AccessSet READERS = gpu::Access::sampled_fragment | gpu::Access::sampled_compute;

void MotionFieldPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    // Nothing moves and the marks have faded: the map is left as it is.
    if (moving_count_ == 0 && settled_) { return; }
    // Last frame's map carried forward into this frame's. The graph moves
    // the one written into GENERAL, and back to rest afterwards.
    builder.read(builder.import_named(ctx, "motion_field_prev"),
                 gpu::Access::sampled_compute);
    builder.write(builder.import_named(ctx, "motion_field"), gpu::Access::storage_write_compute);
}

void MotionFieldPass::collect_exports(DeclareContext& ctx) {
    if (!maps_[cur_]) { return; }
    // Both maps rest where their readers sample them, and are carried into
    // the next frame's map.
    const TextureExport how{.persistent = true, .resting = READERS};
    // The map this frame writes is the one the global set binds.
    ctx.export_texture("motion_field", maps_[cur_], how);
    ctx.export_texture("motion_field_prev", maps_[prev_index()], how);
}

void MotionFieldPass::record(const FrameContext& ctx) {
    FJELL_PROFILE_SCOPE_N("record_motion_field");
    if (moving_count_ == 0 && settled_) return;
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Motion Field");

    // prepare_frame chose which map this frame writes, so that readers could
    // be pointed at the other one before any of this was recorded.
    const uint32_t write = cur_;
    const uint32_t read = 1 - cur_;

    const float texel = EXTENT_METRES / static_cast<float>(RESOLUTION);
    // How far the map slid since last frame, in texels: where this frame's
    // texel sits, expressed in the previous frame's grid. Both origins are
    // snapped, so this is a whole number and the read lands on a texel centre
    // rather than between two.
    const glm::vec2 scroll = (origin_ - prev_origin_) / texel;

    // A storage buffer holds at least one source, even in a frame with none.
    const auto sources = cmd.transient(std::span(sources_.data(), std::max(source_count_, 1U)));
    cmd.set_pipeline(pipeline_);
    cmd.bind({{"SourceBuffer", gpu::storage(sources)},
              {"motion_prev", gpu::sampled(maps_[read], sampler_)},
              {"motion_out", gpu::storage(maps_[write])}});
    cmd.push(PushConstants{
        .map_origin_extent = glm::vec4(origin_.x, origin_.y, EXTENT_METRES, texel),
        .scroll_decay = glm::vec4(scroll.x, scroll.y, 1.0f / MEMORY_SECONDS, ctx.delta_time),
        .source_count = source_count_,
        .resolution = RESOLUTION,
        .spread = SPREAD_PER_SECOND,
        .wake_width = WAKE_WIDTH,
        .swirl = SWIRL_METRES_PER_SECOND,
        .time = elapsed_,
    });
    const uint32_t groups = (RESOLUTION + 7) / 8;
    cmd.dispatch(groups, groups, 1);
    first_frame_ = false;
    // Settled once nothing has moved for as long as the map remembers: by
    // then the last mark has decayed to nothing and further frames would
    // write the same empty map again.
    elapsed_ += ctx.delta_time;
    still_seconds_ = moving_count_ > 0 ? 0.0f : still_seconds_ + ctx.delta_time;
    settled_ = still_seconds_ >= MEMORY_SECONDS;
}

} // namespace fjell
