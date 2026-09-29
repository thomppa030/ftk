#include "renderer/pass/meshlet_cull_pass2.hpp"

#include "core/profiler.hpp"
#include "renderer/indirect_batch.hpp"
#include "renderer/pass/meshlet_instance_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/pass_registry.hpp"

namespace fjell {

void MeshletCullPass2::post_create(GpuContext& /*gpu*/, PassRegistry& passes) {
    instance_pass_ = &passes.get<MeshletInstancePass>("meshlet_cull");
}

PassSetup MeshletCullPass2::setup() const {
    return {
        .name = "meshlet_cull_pass2",
        .phase = RenderPhase::DepthPrepass,
        .sub_order = 150,
    };
}

void MeshletCullPass2::declare(PassBuilder& builder, const DeclareContext& ctx) {
    if (auto hiz = builder.import_named_optional(ctx, "hiz_pyramid"); hiz.valid()) {
        builder.read(hiz, gpu::Access::sampled_compute);
    }
    // Tests again what the first cull set aside, and writes what is now
    // visible into the second set of the same instances and commands.
    builder.read(builder.import_named_buffer(ctx, "meshlet_culled"),
                 gpu::Access::storage_buffer_read_compute);
    builder.read(builder.import_named_buffer(ctx, "meshlet_culled_count"),
                 gpu::Access::storage_buffer_read_compute);
    builder.read_write(builder.import_named_buffer(ctx, "meshlet_visible"),
                       gpu::Access::storage_buffer_read_write_compute);
    builder.read_write(builder.import_named_buffer(ctx, "meshlet_indirect"),
                       gpu::Access::storage_buffer_read_write_compute);
}

void MeshletCullPass2::record(const FrameContext& ctx) {
    FJELL_PROFILE_SCOPE_N("record_meshlet_cull2");
    if (instance_pass_ == nullptr || ctx.indirect_batch == nullptr) { return; }
    if (ctx.indirect_batch->opaque_draw_count(ctx.frame_index) == 0) { return; }
    auto zone = ctx.commands().zone("Meshlet Cull Pass 2");
    instance_pass_->record_cull(ctx, MeshletInstancePass::Cull::second);
}

} // namespace fjell
