#include "renderer/pass/meshlet_cull_pass2.hpp"
#include "gpu/vulkan/native.hpp"
#include "core/profiler.hpp"
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
    // Tests again what the first cull set aside, and appends what is now
    // visible to the same instances and indirect commands.
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
#ifdef FJELL_ENABLE_TRACY
    TracyVkZone(static_cast<tracy::VkCtx*>(ctx.tracy_ctx), ctx.cmd, "Meshlet Cull Pass 2");
#endif
    if (instance_pass_ == nullptr) { return; }
    if (ctx.indirect_batch == nullptr) { return; }

    uint32_t total_draws = ctx.indirect_batch->opaque_draw_count(ctx.frame_index);
    if (total_draws == 0) { return; }

    auto* hiz_imgs = ctx.pp_resources ? ctx.pp_resources->hiz() : nullptr;
    bool hiz_ready = hiz_imgs != nullptr && hiz_imgs->ready();
    if (!hiz_ready) { return; }

    auto& ranges_ref = ctx.indirect_batch->pipeline_ranges(ctx.frame_index);
    auto extent = ctx.target->extent();

    VkBuffer bounds_buf = VK_NULL_HANDLE;
    VkDeviceSize bounds_size = 0;
    if (ctx.registry != nullptr) {
        if (auto* bounds = ctx.registry->find_buffer("meshlet_bounds")) {
            bounds_buf = bounds->buffer;
            bounds_size = bounds->size;
        }
    }

    VkSampler hiz_sampler = VK_NULL_HANDLE;
    if (ctx.passes != nullptr) {
        if (auto* hiz = ctx.passes->try_get<HizPass>("hiz")) {
            hiz_sampler = gpu::vulkan::native_sampler(*ctx.device, hiz->sampler());
        }
    }

    auto cb = ctx.cmd;
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
        gpu::vulkan::native_view(*ctx.device, hiz_imgs->pyramid), hiz_sampler,
        extent.width, extent.height,
        hiz_imgs->mip_count);
}

} // namespace fjell
