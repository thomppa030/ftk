#include "renderer/pass/hiz_pass.hpp"

#include "core/log.hpp"
#include "core/profiler.hpp"
#include "gpu/device.hpp"
#include "gpu/vulkan/native.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/post_process_resources.hpp"
#include "renderer/resource_registry.hpp"

#include <algorithm>
#include <cmath>

namespace fjell {

// ── Images (per-viewport state) ─────────────────────────────────────────────

void HizPass::Images::create(gpu::Device& gpu, uint32_t depth_width, uint32_t depth_height) {
    // Hi-Z mip 0 = half-res
    width = std::max(depth_width / 2, 1U);
    height = std::max(depth_height / 2, 1U);
    mip_count = static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1;
    mip_count = std::min(mip_count, MAX_HIZ_MIPS);
    auto made = gpu.create(gpu::TextureDesc{
        .format = gpu::Format::r32_float,
        .width = width,
        .height = height,
        .mips = mip_count,
        .use = gpu::TextureUse::storage | gpu::TextureUse::sampled,
        .name = "hiz_pyramid",
    });
    if (!made) {
        FJELL_GFX_ERROR("{}", made.error());
        destroy();
        return;
    }
    pyramid = std::move(*made);
}

void HizPass::Images::destroy() {
    pyramid.reset();
    mip_count = 0;
    width = 0;
    height = 0;
}

void HizPass::Images::resize(gpu::Device& gpu, uint32_t depth_width, uint32_t depth_height) {
    const uint32_t new_w = std::max(depth_width / 2, 1U);
    const uint32_t new_h = std::max(depth_height / 2, 1U);
    if (new_w == width && new_h == height && ready()) return;
    destroy();
    create(gpu, depth_width, depth_height);
}

// ── HizPass (singleton) ─────────────────────────────────────────────────────

Result<> HizPass::create(Device& /*device*/, const PassResources& resources) {
    device_ = resources.device;
    sampler_ = device_->sampler({.filter = gpu::Filter::nearest,
                                 .mip_filter = gpu::Filter::nearest,
                                 .address = gpu::Address::clamp});
    FJELL_TRY(pipeline_, device_->create(gpu::ComputePipelineDesc{
                             .shader = "shaders/hiz_build.comp",
                             .name = "hiz_build",
                         }));
    FJELL_GFX_DEBUG("HizPass created");
    return {};
}

void HizPass::destroy() {
    default_images_.destroy();
    pipeline_.reset();
}

void HizPass::resize(uint32_t width, uint32_t height) {
    default_images_.resize(*device_, width, height);
}

void HizPass::record(const FrameContext& ctx) {
    FJELL_PROFILE_SCOPE_N("record_hiz");
    // Prefer the viewport's per-instance Images (SceneViewport uses its
    // own PostProcessResources); fall back to default_images_ when the
    // caller doesn't provide a pp_resources bundle.
    auto* imgs = ctx.pp_resources ? ctx.pp_resources->hiz() : nullptr;
    Images* target = imgs != nullptr && imgs->ready() ? imgs
                   : default_images_.ready()            ? &default_images_
                                                        : nullptr;
    if (target == nullptr) return;
    auto zone = ctx.commands().zone("Hi-Z Build");
    record_into(ctx, *target);
}

void HizPass::pre_record(const FrameContext& ctx, GpuContext& /*gpu*/) {
    auto* imgs = ctx.pp_resources ? ctx.pp_resources->hiz() : nullptr;
    active_images_ = imgs;
    if (!imgs || !ctx.target) return;
    const auto extent = ctx.target->extent();
    if (extent.width == 0 || extent.height == 0) return;
    imgs->resize(*device_, extent.width, extent.height);
}

void HizPass::register_resources(ResourceRegistry& registry) {
    if (active_images_ && active_images_->ready()) {
        registry.register_image("hiz", {
            .frame_graph_id = 0,
            .texture = active_images_->pyramid,
            .image = gpu::vulkan::native_image(*device_, active_images_->pyramid),
            .view = gpu::vulkan::native_view(*device_, active_images_->pyramid),
            .sampler = gpu::vulkan::native_sampler(*device_, sampler_),
            .mip_count = active_images_->mip_count});
    }
}

void HizPass::record_into(const FrameContext& ctx, Images& imgs) {
    const auto extent = ctx.target->extent();
    if (extent.width == 0 || extent.height == 0 || imgs.mip_count == 0) return;

    // The graph has every mip of the pyramid written as storage. Each mip
    // is reduced from the one before, depth for the first, and handed to
    // the next as a compute read; the chain ends sampled in compute, where
    // the graph expects it.
    auto& cmd = ctx.commands();
    cmd.set_pipeline(pipeline_);
    uint32_t src_w = extent.width;
    uint32_t src_h = extent.height;
    for (uint32_t mip = 0; mip < imgs.mip_count; ++mip) {
        const uint32_t dst_w = std::max(imgs.width >> mip, 1U);
        const uint32_t dst_h = std::max(imgs.height >> mip, 1U);
        const gpu::TextureView source = mip == 0 ? gpu::TextureView(ctx.target->depth_texture())
                                                 : gpu::mip(imgs.pyramid, mip - 1);
        cmd.bind({{"src_depth", gpu::sampled(source, sampler_)},
                  {"dst_mip", gpu::storage(gpu::mip(imgs.pyramid, mip))}});
        cmd.push(glm::uvec4(src_w, src_h, dst_w, dst_h));
        cmd.dispatch((dst_w + 7) / 8, (dst_h + 7) / 8, 1);
        cmd.barrier(gpu::mip(imgs.pyramid, mip), gpu::Access::storage_write_compute,
                    gpu::Access::sampled_compute);
        src_w = dst_w;
        src_h = dst_h;
    }
}

PassSetup HizPass::setup() const {
    // On the graphics queue: the second meshlet cull reads the pyramid and
    // its depth draw writes the depth the compute queue's passes read, and
    // the compute queue runs one stretch of a frame's work, which cannot
    // wait on graphics work that waits on it.
    return {
        .name = "hiz",
        .phase = RenderPhase::DepthPrepass,
        .sub_order = 100,
    };
}

void HizPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    auto depth = builder.import_named(ctx, "depth");
    builder.read(depth, gpu::Access::sampled_compute);

    auto hiz = builder.import_named(ctx, "hiz_pyramid");
    if (hiz.valid()) {
        // record_into() writes every mip of the pyramid as a storage
        // image, each handed to the next mip's reduction as a compute read,
        // and leaves the whole chain sampled in compute: downstream
        // consumers (MeshletCullPass2, SSR) need no further barrier.
        builder.write(hiz, gpu::Access::storage_write_compute);
        builder.leaves(hiz, gpu::Access::storage_write_compute, gpu::Access::sampled_compute);
    }
    // Keep Hi-Z alive independently of whether the declared write
    // is wired this frame — MeshletCullPass2 / cull viz / SSR reach
    // the pyramid through pass-private descriptor writes.
    builder.has_side_effects();
}

} // namespace fjell
