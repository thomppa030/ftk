#include "renderer/pass/rt_scene_pass.hpp"

#include "core/profiler.hpp"
#include "renderer/accel_structure.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/render_stats.hpp"
#include "scene/scene.hpp"

namespace fjell {

Result<> RtScenePass::create(const PassResources& /*resources*/) {
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

void RtScenePass::collect_exports(DeclareContext& ctx) {
    if (accel_manager_ == nullptr) return;
    const gpu::AccelerationStructure tlas = accel_manager_->prepared_tlas(ctx.scene_id, ctx.frame_index);
    if (!tlas.valid()) return;
    ctx.buffer_imports.emplace(fg_name_hash(AccelStructureManager::TLAS_NAME),
                               BufferImport{.structure = tlas});
    ctx.buffer_imports.emplace(
        fg_name_hash(AccelStructureManager::INSTANCES_NAME),
        BufferImport{.buffer = accel_manager_->prepared_instances(ctx.scene_id, ctx.frame_index)});
}

void RtScenePass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    // The build reads the simulated particles for the mesh-particle instances.
    builder.read(builder.import_named_buffer(ctx, "particle_state"),
                 gpu::Access::storage_buffer_read_compute);
    // A viewport after the first that renders the scene builds nothing and
    // says it writes all the same: an order, never a hazard.
    builder.write(builder.import_named_acceleration(ctx, AccelStructureManager::TLAS_NAME),
                  gpu::Access::acceleration_build);
    // The hit data, which the particles' instances write on the GPU.
    builder.write(builder.import_named_buffer(ctx, AccelStructureManager::INSTANCES_NAME),
                  gpu::Access::storage_buffer_write_compute);
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
