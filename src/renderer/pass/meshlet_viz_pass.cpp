#include "renderer/pass/meshlet_viz_pass.hpp"

#include "core/log.hpp"
#include "core/profiler.hpp"
#include "gpu/device.hpp"
#include "renderer/indirect_batch.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/render_queue.hpp"
#include "renderer/viewport_source.hpp"

#include <algorithm>

namespace fjell {

namespace {

constexpr gpu::Format VIZ_FORMAT = gpu::Format::rgba8_unorm;
constexpr const char* DEPTH_NAME = "meshlet_viz_depth";

} // namespace

Result<> MeshletVizPass::create(Device& /*device*/, const PassResources& resources) {
    gpu::Device& device = *resources.device;
    depth_format_ = resources.scene_depth_format;
    viz_image_.init(device, VIZ_FORMAT, "viz_meshlets");
    // Every side drawn, nearer wins, into its own single-sample image.
    FJELL_TRY(pipeline_, device.create(gpu::GraphicsPipelineDesc{
                             .task = "shaders/depth_mesh.task",
                             .mesh = "shaders/meshlet_viz.mesh",
                             .fragment = "shaders/meshlet_viz.frag",
                             .depth = {.test = true, .write = true, .compare = gpu::Compare::less},
                             .color = {{VIZ_FORMAT, gpu::Blend::none}},
                             .depth_format = depth_format_,
                             .shared = {resources.globals, resources.draw_data, resources.meshlets},
                             .name = "meshlet_viz",
                         }));
    FJELL_GFX_DEBUG("Meshlet viz pass created");
    return {};
}

void MeshletVizPass::destroy() {
    viz_image_.cleanup();
    pipeline_.reset();
}

void MeshletVizPass::record(const FrameContext& ctx) {
    const gpu::Texture depth = ctx.texture(DEPTH_NAME);
    if (!viz_image_.valid() || !depth.valid()) return;

    FJELL_PROFILE_SCOPE_N("record_viz_meshlet_viz");
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Viz: Meshlet");
    gpu::RenderTargets targets = viz_image_.targets(gpu::Clear{0.05f, 0.05f, 0.07f, 1.0f});
    targets.depth = gpu::DepthAttachment{.view = depth, .load = gpu::Load::clear, .store = gpu::Store::discard};
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
        const gpu::Buffer commands = batch.mesh_indirect_buffer(ctx.frame_index);
        const gpu::Buffer counts = batch.mesh_count_buffer(ctx.frame_index);
        const uint32_t most_groups = std::max(batch.most_mesh_task_groups(ctx.frame_index), 1U);
        const auto& ranges = batch.pipeline_ranges(ctx.frame_index);
        for (uint32_t r = 0; r < static_cast<uint32_t>(ranges.size()); ++r) {
            const uint32_t first_draw = ranges[r].first_draw;
            pass.push(first_draw);
            pass.draw_mesh_tasks_indirect_count(
                gpu::BufferRange(commands, uint64_t{first_draw} * sizeof(gpu::DrawMeshTasksArgs),
                                 uint64_t{ranges[r].draw_count} * sizeof(gpu::DrawMeshTasksArgs)),
                gpu::BufferRange(counts, uint64_t{r} * sizeof(uint32_t), sizeof(uint32_t)),
                ranges[r].draw_count, sizeof(gpu::DrawMeshTasksArgs), most_groups);
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

void MeshletVizPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    if ((ctx.active_sources & (1U << static_cast<uint32_t>(ViewportSource::meshlet_viz))) == 0U) {
        return;
    }
    if (!viz_image_.declare(builder, ctx)) return;
    // The view's own depth, the viewport's size as its image is, dropped
    // once drawn.
    TextureDesc depth_desc{};
    depth_desc.format = depth_format_;
    depth_desc.size_class = SizeClass::viewport;
    builder.write(builder.create(DEPTH_NAME, depth_desc), gpu::Access::depth_attachment);
}

void MeshletVizPass::collect_exports(DeclareContext& ctx) {
    viz_image_.collect_exports(ctx);
}

} // namespace fjell
