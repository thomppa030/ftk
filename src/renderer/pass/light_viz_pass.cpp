#include "renderer/pass/light_viz_pass.hpp"

#include "core/log.hpp"
#include "core/profiler.hpp"
#include "gpu/device.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/viewport_source.hpp"

namespace fjell {

namespace {

struct LightVizPush {
    uint32_t tiles_x;
    uint32_t tiles_y;
    uint32_t mode;
};

} // namespace

Result<> LightVizPass::create(const PassResources& resources) {
    gpu::Device& device = *resources.device;
    viz_image_.init(device, gpu::Format::rgba8_srgb, mode_ == 0 ? "viz_light_heatmap" : "viz_light_grid");
    FJELL_TRY(pipeline_, device.create(gpu::GraphicsPipelineDesc{
                             .vertex = "shaders/light_heatmap.vert",
                             .fragment = "shaders/light_heatmap.frag",
                             .color = {{gpu::Format::rgba8_srgb, gpu::Blend::none}},
                             .name = "light_viz",
                         }));
    FJELL_GFX_DEBUG("Light visualization pass created");
    return {};
}

void LightVizPass::destroy() {
    viz_image_.cleanup();
    pipeline_.reset();
}

void LightVizPass::record(const FrameContext& ctx) {
    if (!ctx.target || !viz_image_.valid()) return;
    if (!tile_buffer_.valid()) return;

    FJELL_PROFILE_SCOPE_N("record_viz_light_viz");
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Viz: Light");
    auto pass = cmd.render(viz_image_.targets());
    pass.set_pipeline(pipeline_);
    pass.bind({{"TileLightData", gpu::storage(tile_buffer_)}});
    const auto target_extent = ctx.target->extent();
    pass.push(LightVizPush{
        .tiles_x = (target_extent.x + 15) / 16,
        .tiles_y = (target_extent.y + 15) / 16,
        .mode = mode_,
    });
    pass.draw(3);
    viz_image_.mark_rendered();
}

PassSetup LightVizPass::setup() const {
    return {
        .name = (mode_ == 0) ? "light_heatmap" : "light_grid",
        .phase = RenderPhase::Debug,
        .sub_order = (mode_ == 0) ? 60u : 70u,
    };
}

void LightVizPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    // Registered twice, once per view; each instance draws only while its
    // own view is showing.
    tile_buffer_ = {};
    const auto source = mode_ == 0 ? ViewportSource::light_heatmap : ViewportSource::light_grid;
    if ((ctx.active_sources & (1U << static_cast<uint32_t>(source))) == 0U) { return; }
    const auto tiles = ctx.buffer_imports.find(fg_name_hash("tile_light_ssbo"));
    if (tiles == ctx.buffer_imports.end()) { return; }
    tile_buffer_ = tiles->second.buffer;
    builder.read(builder.import_named_buffer(ctx, "tile_light_ssbo"),
                 gpu::Access::storage_buffer_read_fragment);
    viz_image_.declare(builder, ctx);
}

void LightVizPass::collect_exports(DeclareContext& ctx) {
    viz_image_.collect_exports(ctx);
}

} // namespace fjell
