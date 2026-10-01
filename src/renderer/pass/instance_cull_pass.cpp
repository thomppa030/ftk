#include "renderer/pass/instance_cull_pass.hpp"

#include "ftk/base/log.hpp"
#include "ftk/base/profiler.hpp"
#include "ftk/framegraph/pass_builder.hpp"
#include "ftk/gpu/device.hpp"
#include "renderer/indirect_batch.hpp"

namespace fjell {

ftk::Result<> InstanceCullPass::create(const PassResources& resources) {
    // Set 0 is the globals (the view-projection); set 1 the pass's buffers.
    FTK_TRY(pipeline_, resources.device->create(ftk::gpu::ComputePipelineDesc{
                             .shader = "shaders/instance_cull.comp",
                             .shared = {resources.globals},
                             .name = "instance_cull",
                         }));
    FTK_GFX_DEBUG("InstanceCullPass created");
    return {};
}

void InstanceCullPass::destroy() {
    pipeline_.reset();
}

PassSetup InstanceCullPass::setup() const {
    return {
        .name = "instance_cull",
        .phase = RenderPhase::DepthPrepass,
        .sub_order = 10,  // after meshlet_cull (sub_order 0), before depth draw
    };
}

void InstanceCullPass::pre_record(const FrameContext& ctx, GpuContext& /*gpu*/) {
    transforms_ = {};
    commands_ = {};
    if (ctx.indirect_batch == nullptr
        || ctx.indirect_batch->instanced_draw_count(ctx.frame_index) == 0) {
        return;
    }
    // The graph follows whole buffers: the commands' is the frame memory
    // they were written into.
    transforms_ = {.buffer = ctx.indirect_batch->visible_instances(ctx.frame_index).buffer};
    commands_ = {.buffer = ctx.indirect_batch->indirect_commands(ctx.frame_index).buffer};
}

void InstanceCullPass::collect_exports(DeclareContext& ctx) {
    // What the vertex path draws instanced geometry from: the transforms
    // that survived the cull, and the commands whose instance counts say
    // how many did.
    if (transforms_.buffer.valid()) {
        ctx.buffer_imports.emplace(ftk::fg_name_hash("instance_transforms"), transforms_);
    }
    if (commands_.buffer.valid()) {
        ctx.buffer_imports.emplace(ftk::fg_name_hash("batch_indirect"), commands_);
    }
}

void InstanceCullPass::declare(ftk::PassBuilder& builder, const DeclareContext& ctx) {
    builder.write(builder.import_named_buffer(ctx, "instance_transforms"),
                  ftk::gpu::Access::storage_buffer_write_compute);
    // The instance counts are patched in place in commands the CPU wrote.
    builder.read_write(builder.import_named_buffer(ctx, "batch_indirect"),
                       ftk::gpu::Access::storage_buffer_read_write_compute);
}

void InstanceCullPass::record(const FrameContext& ctx) {
    FTK_PROFILE_SCOPE_N("record_instance_cull");
    if (ctx.indirect_batch == nullptr) { return; }
    const IndirectBatch& batch = *ctx.indirect_batch;
    const uint32_t frame = ctx.frame_index;
    const uint32_t draw_count = batch.instanced_draw_count(frame);
    if (draw_count == 0) { return; }

    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Instance Cull");
    cmd.set_pipeline(pipeline_);
    cmd.bind(ctx.globals);
    // Each instanced draw's box and slice, the transforms the CPU wrote, the
    // compacted survivors, and the commands whose instance counts are patched.
    cmd.bind({{"InstancedDrawBuffer", ftk::gpu::storage(batch.instanced_draws(frame))},
              {"InstanceTransformsBuffer", ftk::gpu::storage(batch.instance_transforms(frame))},
              {"VisibleTransformsBuffer", ftk::gpu::storage(batch.visible_instances(frame))},
              {"IndirectBuffer", ftk::gpu::storage(batch.indirect_commands(frame))}});
    cmd.dispatch(draw_count, 1, 1);
}

} // namespace fjell
