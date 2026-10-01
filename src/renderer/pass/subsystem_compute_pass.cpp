#include "renderer/pass/subsystem_compute_pass.hpp"

#include "ftk/base/profiler.hpp"
#include "ftk/framegraph/pass_builder.hpp"
#include "renderer/systems/render_subsystem.hpp"

namespace fjell {

ftk::Result<> SubsystemComputePass::create(const PassResources& /*resources*/) {
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

void SubsystemComputePass::declare(ftk::PassBuilder& builder, const DeclareContext& ctx) {
    // Everything the subsystems simulate in this run and then draw. A
    // buffer that carries state into the next frame is persistent, which
    // keeps the simulation running while nothing on screen draws it.
    for (const auto& input : ctx.subsystem_draw_inputs) {
        if (!input.written) continue;
        builder.read_write(builder.import("subsystem_draw_input", input.buffer.buffer,
                                          input.buffer.persistent),
                           ftk::gpu::Access::storage_buffer_read_write_compute);
    }
}

void SubsystemComputePass::record(const FrameContext& ctx) {
    FTK_PROFILE_SCOPE_N("record_subsystem_compute");
    // Each subsystem records its dispatches and barriers into this pass's
    // list.
    auto zone = ctx.commands().zone("Subsystem Compute");
    for (uint32_t i = 0; i < ctx.subsystem_count; ++i) {
        ctx.subsystems[i]->record_compute(ctx);
    }
}

} // namespace fjell
