#include "renderer/pass/second_cull_depth_pass.hpp"

#include "core/profiler.hpp"
#include "gpu/render_encoder.hpp"
#include "renderer/indirect_batch.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass/mesh_shader_depth_pass.hpp"
#include "renderer/pass/meshlet_instance_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/pass_registry.hpp"

namespace fjell {

void SecondCullDepthPass::post_create(GpuContext& /*gpu*/, PassRegistry& passes) {
    depth_pass_ = passes.try_get<MeshShaderDepthPass>("mesh_shader_depth");
}

PassSetup SecondCullDepthPass::setup() const {
    return {
        .name = "second_cull_depth",
        .phase = RenderPhase::DepthPrepass,
        .sub_order = 160,
    };
}

void SecondCullDepthPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    builder.read(builder.import_named_buffer(ctx, "meshlet_visible"),
                 gpu::Access::storage_buffer_read_mesh);
    builder.read(builder.import_named_buffer(ctx, "meshlet_indirect"),
                 gpu::Access::indirect_read);
    // Tested against and added to what the prepass drew, which
    // DepthResolvePass resolves afterwards; without MSAA the two names are
    // one attachment.
    const bool has_msaa = ctx.msaa_samples != gpu::Samples::x1;
    builder.write(builder.import_named(ctx, has_msaa ? "msaa_depth" : "depth"),
                  gpu::Access::depth_attachment);
}

void SecondCullDepthPass::record(const FrameContext& ctx) {
    FJELL_PROFILE_SCOPE_N("record_second_cull_depth");
    if (depth_pass_ == nullptr || ctx.target == nullptr || ctx.indirect_batch == nullptr) return;
    // Meshlets read the meshlet set and the cull's commands, which a
    // viewport graph without the meshlet cull does not have.
    if (!ctx.gpu_driven_commands.valid() || !ctx.meshlets.valid()) return;
    if (ctx.indirect_batch->opaque_draw_count(ctx.frame_index) == 0) return;

    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Second Cull Depth");
    auto pass = cmd.render({.depth = gpu::DepthAttachment{.view = ctx.target->msaa_depth_texture()}});
    depth_pass_->draw_found(pass, ctx, MeshletInstancePass::Cull::second);
}

} // namespace fjell
