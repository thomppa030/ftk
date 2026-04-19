#include "renderer/pass/meshlet_cull_pass2.hpp"
#include "renderer/pass/meshlet_instance_pass.hpp"
#include "renderer/pass/hiz_pass.hpp"
#include "renderer/indirect_batch.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/pass_registry.hpp"
#include "renderer/post_process_resources.hpp"
#include "renderer/resource_registry.hpp"

#ifdef FJELL_ENABLE_TRACY
#include <tracy/TracyVulkan.hpp>
#endif

namespace fjell {

PassSetup MeshletCullPass2::setup() const {
    return {
        .name = "meshlet_cull_pass2",
        .phase = RenderPhase::DepthPrepass,
        .sub_order = 150,
        .reads = {},
        .writes = {},
    };
}

void MeshletCullPass2::declare(PassBuilder& /*builder*/, const DeclareContext& /*ctx*/) {
    // Hi-Z image read and pass-private SSBO writes are all hand-
    // barriered inside record. Graph sees an empty-access pass.
}

void MeshletCullPass2::build(FrameGraph& graph, ResourceRegistry& registry,
                              const FrameContext& ctx) {
    if (!instance_pass_) { return; }

    PassBuilder builder;
    graph.submit_declared_pass("meshlet_cull_pass2", builder,
        [this, ctx, &registry](VkCommandBuffer cb) {
#ifdef FJELL_ENABLE_TRACY
        TracyVkZone(static_cast<tracy::VkCtx*>(ctx.tracy_ctx), cb, "Meshlet Cull Pass 2");
#endif
        if (!ctx.use_mesh_shaders || !ctx.indirect_batch) return;

        uint32_t total_draws = ctx.indirect_batch->opaque_draw_count(ctx.frame_index);
        if (total_draws == 0) return;

        auto* hiz_imgs = ctx.pp_resources ? ctx.pp_resources->hiz() : nullptr;
        bool hiz_ready = hiz_imgs && hiz_imgs->ready();
        if (!hiz_ready) return; // no Hi-Z = nothing to retest against

        auto& ranges_ref = ctx.indirect_batch->pipeline_ranges(ctx.frame_index);
        auto extent = ctx.target->extent();

        VkBuffer bounds_buf = VK_NULL_HANDLE;
        VkDeviceSize bounds_size = 0;
        if (auto* bounds = registry.find_buffer("meshlet_bounds")) {
            bounds_buf = bounds->buffer;
            bounds_size = bounds->size;
        }

        VkSampler hiz_sampler = VK_NULL_HANDLE;
        if (ctx.passes) {
            if (auto* hiz = ctx.passes->try_get<HizPass>("hiz")) {
                hiz_sampler = hiz->sampler();
            }
        }

        instance_pass_->dispatch_cull_second(
            cb,
            ctx.global_descriptor_set,
            ctx.draw_data_set,
            ctx.indirect_batch->draw_data_buffer(ctx.frame_index),
            ctx.indirect_batch->draw_data_size(ctx.frame_index),
            bounds_buf, bounds_size,
            ctx.indirect_batch->pipeline_bin_buffer(ctx.frame_index),
            ctx.indirect_batch->pipeline_bin_size(ctx.frame_index),
            static_cast<uint32_t>(ranges_ref.size()),
            ctx.frame_index,
            hiz_imgs->hiz_sampled_view, hiz_sampler,
            extent.width, extent.height,
            hiz_imgs->hiz_mip_count);

        instance_pass_->insert_cull_barrier(cb);
    });
}

} // namespace fjell
