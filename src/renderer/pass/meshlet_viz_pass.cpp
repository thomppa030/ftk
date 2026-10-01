#include "renderer/pass/meshlet_viz_pass.hpp"

#include "ftk/base/log.hpp"
#include "ftk/base/profiler.hpp"
#include "ftk/framegraph/pass_builder.hpp"
#include "ftk/gpu/device.hpp"
#include "renderer/indirect_batch.hpp"
#include "renderer/render_queue.hpp"
#include "renderer/viewport_source.hpp"

#include <algorithm>

namespace fjell {

namespace {

constexpr ftk::gpu::Format VIZ_FORMAT = ftk::gpu::Format::rgba8_unorm;
constexpr const char* DEPTH_NAME = "meshlet_viz_depth";

} // namespace

ftk::Result<> MeshletVizPass::create(const PassResources& resources) {
    ftk::gpu::Device& device = *resources.device;
    depth_format_ = resources.scene_depth_format;
    viz_image_.init(device, VIZ_FORMAT, "viz_meshlets");
    // Every side drawn, nearer wins, into its own single-sample image.
    FTK_TRY(pipeline_, device.create(ftk::gpu::GraphicsPipelineDesc{
                             .task = "shaders/depth_mesh.task",
                             .mesh = "shaders/meshlet_viz.mesh",
                             .fragment = "shaders/meshlet_viz.frag",
                             .depth = {.test = true, .write = true, .compare = ftk::gpu::Compare::less},
                             .color = {{VIZ_FORMAT, ftk::gpu::Blend::none}},
                             .depth_format = depth_format_,
                             .shared = {resources.globals, resources.draw_data, resources.meshlets},
                             .name = "meshlet_viz",
                         }));
    FTK_GFX_DEBUG("Meshlet viz pass created");
    return {};
}

void MeshletVizPass::destroy() {
    viz_image_.cleanup();
    pipeline_.reset();
}

void MeshletVizPass::record(const FrameContext& ctx) {
    const ftk::gpu::Texture depth = ctx.texture(DEPTH_NAME);
    if (!viz_image_.valid() || !depth.valid()) return;

    FTK_PROFILE_SCOPE_N("record_viz_meshlet_viz");
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Viz: Meshlet");
    ftk::gpu::RenderTargets targets = viz_image_.targets(ftk::gpu::Clear{0.05f, 0.05f, 0.07f, 1.0f});
    targets.depth = ftk::gpu::DepthAttachment{.view = depth, .load = ftk::gpu::Load::clear, .store = ftk::gpu::Store::discard};
    auto pass = cmd.render(targets);

    // The meshlets read the meshlet set, which a viewport graph without the
    // meshlet cull does not bind.
    const bool can_draw = ctx.target && ctx.queue && !ctx.queue->empty() && ctx.meshlets.valid()
                          && ctx.indirect_batch;
    if (can_draw) {
        const IndirectBatch& batch = *ctx.indirect_batch;
        pass.set_pipeline(pipeline_);
        pass.bind(ctx.globals);
        pass.bind(ctx.draw_data);
        pass.bind(ctx.meshlets);
        const uint32_t most_groups = std::max(batch.most_mesh_task_groups(ctx.frame_index), 1U);
        const auto& ranges = batch.pipeline_ranges(ctx.frame_index);
        for (uint32_t r = 0; r < static_cast<uint32_t>(ranges.size()); ++r) {
            const uint32_t first_draw = ranges[r].first_draw;
            pass.push(first_draw);
            pass.draw_mesh_tasks_indirect_count(batch.mesh_commands(ctx.frame_index, first_draw, ranges[r].draw_count),
                                                batch.mesh_count(ctx.frame_index, r), ranges[r].draw_count,
                                                sizeof(ftk::gpu::DrawMeshTasksArgs), most_groups);
        }
    }
    viz_image_.mark_rendered();
}

PassSetup MeshletVizPass::setup() const {
    return {
        .name = "meshlet_viz",
        .phase = RenderPhase::Debug,
        .sub_order = 50,
    };
}

void MeshletVizPass::declare(ftk::PassBuilder& builder, const DeclareContext& ctx) {
    if ((ctx.active_sources & (1U << static_cast<uint32_t>(ViewportSource::meshlet_viz))) == 0U) {
        return;
    }
    if (!viz_image_.declare(builder, ctx)) return;
    // The view's own depth, the viewport's size as its image is, dropped
    // once drawn.
    ftk::TextureDesc depth_desc{};
    depth_desc.format = depth_format_;
    depth_desc.size_class = ftk::SizeClass::viewport;
    builder.write(builder.create(DEPTH_NAME, depth_desc), ftk::gpu::Access::depth_attachment);
}

void MeshletVizPass::collect_exports(DeclareContext& ctx) {
    viz_image_.collect_exports(ctx);
}

} // namespace fjell
