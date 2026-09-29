#include "renderer/pass/instance_cull_pass.hpp"

#include "core/log.hpp"
#include "core/profiler.hpp"
#include "gpu/device.hpp"
#include "renderer/indirect_batch.hpp"
#include "renderer/pass_builder.hpp"

namespace fjell {

Result<> InstanceCullPass::create(Device& /*device*/, const PassResources& resources) {
    // Set 0 is the globals (the view-projection); set 1 the pass's buffers.
    FJELL_TRY(pipeline_, resources.device->create(gpu::ComputePipelineDesc{
                             .shader = "shaders/instance_cull.comp",
                             .shared = {resources.globals},
                             .name = "instance_cull",
                         }));
    FJELL_GFX_DEBUG("InstanceCullPass created");
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
    transforms_ = {.buffer = ctx.indirect_batch->visible_instance_buffer(ctx.frame_index)};
    commands_ = {.buffer = ctx.indirect_batch->indirect_buffer(ctx.frame_index)};
}

void InstanceCullPass::collect_exports(DeclareContext& ctx) {
    // What the vertex path draws instanced geometry from: the transforms
    // that survived the cull, and the commands whose instance counts say
    // how many did.
    if (transforms_.buffer.valid()) {
        ctx.buffer_imports.emplace(fg_name_hash("instance_transforms"), transforms_);
    }
    if (commands_.buffer.valid()) {
        ctx.buffer_imports.emplace(fg_name_hash("batch_indirect"), commands_);
    }
}

void InstanceCullPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    builder.write(builder.import_named_buffer(ctx, "instance_transforms"),
                  gpu::Access::storage_buffer_write_compute);
    // The instance counts are patched in place in commands the CPU wrote.
    builder.read_write(builder.import_named_buffer(ctx, "batch_indirect"),
                       gpu::Access::storage_buffer_read_write_compute);
}

void InstanceCullPass::record(const FrameContext& ctx) {
    FJELL_PROFILE_SCOPE_N("record_instance_cull");
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
    cmd.bind({{"InstancedDrawBuffer",
               gpu::storage(gpu::BufferRange(batch.instanced_draw_buffer(frame), 0,
                                             batch.instanced_draw_size(frame)))},
              {"InstanceTransformsBuffer",
               gpu::storage(gpu::BufferRange(batch.instance_transform_buffer(frame), 0,
                                             batch.instance_transform_size(frame)))},
              {"VisibleTransformsBuffer",
               gpu::storage(gpu::BufferRange(batch.visible_instance_buffer(frame), 0,
                                             batch.visible_instance_size(frame)))},
              {"IndirectBuffer", gpu::storage(batch.indirect_buffer(frame))}});
    cmd.dispatch(draw_count, 1, 1);
}

} // namespace fjell
