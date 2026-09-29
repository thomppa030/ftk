#include "renderer/pass/rt_scene_pass.hpp"

#include "core/profiler.hpp"
#include "renderer/accel_structure.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/render_stats.hpp"
#include "scene/scene.hpp"

namespace fjell {

Result<> RtScenePass::create(Device& /*device*/, const PassResources& /*resources*/) {
    return {};
}

void RtScenePass::destroy() {}

PassSetup RtScenePass::setup() const {
    return {
        .name = "rt_scene",
        // After the particle simulation (GpuDriven), whose particles become
        // instances, and before DDGI (5), the first pass to trace.
        .phase = RenderPhase::Color,
        .sub_order = 4,
    };
}

void RtScenePass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    // The build reads the simulated particles for the mesh-particle instances.
    builder.read(builder.import_named_buffer(ctx, "particle_state"),
                 gpu::Access::storage_buffer_read_compute);
    // What it writes, the acceleration structure, is not a graph resource:
    // the tracers find it through the manager, so the graph cannot see them
    // depend on this pass.
    builder.has_side_effects();
}

void RtScenePass::record(const FrameContext& ctx) {
    if (accel_manager_ == nullptr || ctx.scene == nullptr) return;
    FJELL_PROFILE_SCOPE_N("record_rt_scene");
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("RT Scene Build");
    const uint32_t instances = accel_manager_->record_build(cmd, ctx.scene->id(), ctx.frame_index);
    if (ctx.render_stats != nullptr) ctx.render_stats->tlas_instances += instances;
}

} // namespace fjell
