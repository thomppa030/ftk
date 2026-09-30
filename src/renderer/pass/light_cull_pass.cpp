#include "renderer/pass/light_cull_pass.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "gpu/device.hpp"
#include "renderer/gpu_context.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/renderer_constants.hpp"

#include <glm/glm.hpp>

namespace fjell {

Result<> LightCullPass::create(Device& /*device*/, const PassResources& resources) {
    FJELL_TRY(pipeline_, resources.device->create(gpu::ComputePipelineDesc{
                             .shader = "shaders/light_cull.comp",
                             .shared = {resources.globals},
                             .name = "light_cull",
                         }));
    FJELL_GFX_DEBUG("Light cull compute pipeline created");
    return {};
}

void LightCullPass::destroy() {
    pipeline_.reset();
}

void LightCullPass::pre_record(const FrameContext& ctx, GpuContext& gpu) {
    output_ = gpu.tile_light_ssbo(ctx.frame_index);
}

void LightCullPass::record(const FrameContext& ctx) {
    FJELL_PROFILE_SCOPE_N("record_light_cull");
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Light Cull");

    const auto extent = ctx.target->extent();
    cmd.set_pipeline(pipeline_);
    cmd.bind(ctx.globals);
    cmd.push(glm::uvec2(extent.x, extent.y));
    const uint32_t groups_x = (extent.x + LIGHT_TILE_SIZE - 1) / LIGHT_TILE_SIZE;
    const uint32_t groups_y = (extent.y + LIGHT_TILE_SIZE - 1) / LIGHT_TILE_SIZE;
    cmd.dispatch(groups_x, groups_y, 1);
}

PassSetup LightCullPass::setup() const {
    return {
        .name = "light_cull",
        .phase = RenderPhase::GpuDriven,
        .sub_order = 110,
        .async_hint = AsyncComputeHint::Prefer,
    };
}

void LightCullPass::collect_exports(DeclareContext& ctx) {
    // This frame slot's tile lists, which pre_record() picked. Every surface
    // shader reads them through the global set; the passes that draw those
    // surfaces import them by this name so the graph sees them wait.
    if (!output_.valid()) { return; }
    ctx.buffer_imports.emplace(fg_name_hash("tile_light_ssbo"),
                               BufferImport{.buffer = output_});
}

void LightCullPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    auto depth = builder.import_named(ctx, "depth");
    builder.read(depth, gpu::Access::sampled_compute);
    // No reader, no reason to run: the opaque and transparent passes read
    // the tile lists, and a viewport without them culls this pass.
    auto tiles = builder.import_named_buffer(ctx, "tile_light_ssbo");
    builder.write(tiles, gpu::Access::storage_buffer_write_compute);
}

} // namespace fjell
