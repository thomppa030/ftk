#include "renderer/pass/subsystem_compute_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/systems/render_subsystem.hpp"

#ifdef FJELL_ENABLE_TRACY
#include <tracy/TracyVulkan.hpp>
#endif

namespace fjell {

Result<> SubsystemComputePass::create(Device& /*device*/, const PassResources& /*resources*/) {
    return {};
}

void SubsystemComputePass::destroy() {}

PassSetup SubsystemComputePass::setup() const {
    return {
        .name = "subsystem_compute",
        .phase = RenderPhase::GpuDriven,
        .sub_order = 120,
        .reads = {},
        .writes = {},
    };
}

void SubsystemComputePass::declare(PassBuilder& /*builder*/, const DeclareContext& /*ctx*/) {
    // Empty graph accesses — subsystem compute dispatches operate on
    // pass-private buffers with hand-coded barriers inside record().
}

void SubsystemComputePass::build(FrameGraph& graph, ResourceRegistry& /*registry*/,
                                  const FrameContext& ctx) {
    PassBuilder builder;
    graph.submit_declared_pass("subsystem_compute", builder,
        [this, ctx](VkCommandBuffer cb) {
            FrameContext local = ctx;
            local.cmd = cb;
            record(local);
        });
}

void SubsystemComputePass::record(const FrameContext& ctx) {
#ifdef FJELL_ENABLE_TRACY
    TracyVkZone(static_cast<tracy::VkCtx*>(ctx.tracy_ctx), ctx.cmd, "Subsystem Compute");
#endif
    for (uint32_t i = 0; i < ctx.subsystem_count; ++i) {
        ctx.subsystems[i]->record_compute(ctx);
    }
}

} // namespace fjell
