#include "renderer/pass/subsystem_compute_pass.hpp"
#include "core/profiler.hpp"
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
    };
}

void SubsystemComputePass::declare(PassBuilder& builder, const DeclareContext& /*ctx*/) {
    // Subsystem compute dispatches update particle/sprite/tilemap SSBOs
    // the graph doesn't track. Flag as side-effecting until Phase 3
    // adds buffer tracking.
    builder.has_side_effects();
}

void SubsystemComputePass::record(const FrameContext& ctx) {
    FJELL_PROFILE_SCOPE_N("record_subsystem_compute");
#ifdef FJELL_ENABLE_TRACY
    TracyVkZone(static_cast<tracy::VkCtx*>(ctx.tracy_ctx), ctx.cmd, "Subsystem Compute");
#endif
    for (uint32_t i = 0; i < ctx.subsystem_count; ++i) {
        ctx.subsystems[i]->record_compute(ctx);
    }
}

} // namespace fjell
