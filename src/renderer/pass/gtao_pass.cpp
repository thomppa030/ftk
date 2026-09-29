// ── Screen-space AO pipeline implementation ─────────────────────────────────
//
// Three RenderPass classes, one compute dispatch each. The FrameGraph emits
// one merged barrier per image per node (RDG-style "merged access state"),
// so nodes declare only the access they need and record no barriers. The
// AO image rests where the global descriptor's "ao_texture" is sampled, and
// the graph returns it there after each node that writes it.

#include "renderer/pass/gtao_pass.hpp"

#include "core/log.hpp"
#include "core/profiler.hpp"
#include "gpu/device.hpp"
#include "gpu/vulkan/native.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/pass_registry.hpp"
#include "renderer/post_process_resources.hpp"

#include <imgui_impl_vulkan.h>

#include <algorithm>

namespace fjell {

namespace {

constexpr uint32_t TILE_SIZE = 8;

struct GtaoPushConstants {
    float radius;
    float intensity;
    float thickness;
    float max_pixel_radius;
    uint32_t viewport_width;
    uint32_t viewport_height;
    uint32_t full_viewport_width;
    uint32_t full_viewport_height;
};

struct BlurPushConstants {
    uint32_t viewport_width;
    uint32_t viewport_height;
    uint32_t direction; // 0 = horizontal, 1 = vertical
    float depth_threshold;
};

/// One blur dispatch over the half-resolution AO, `source` into `output`.
void record_blur(const FrameContext& ctx, AoPass& owner, const AoPass::Images& g,
                 gpu::Texture output, gpu::Texture source, uint32_t direction) {
    auto& cmd = ctx.commands();
    const gpu::Sampler sampler = owner.ao_sampler();
    cmd.set_pipeline(owner.blur_pipeline());
    cmd.bind(ctx.globals);
    cmd.bind({{"blur_output", gpu::storage(output)},
              {"ao_input", gpu::sampled(source, sampler)},
              {"normal_texture", gpu::sampled(ctx.target->normal_texture(), sampler)}});
    cmd.push(BlurPushConstants{
        .viewport_width = g.extent.x,
        .viewport_height = g.extent.y,
        .direction = direction,
        .depth_threshold = 0.1F,
    });
    cmd.dispatch((g.extent.x + TILE_SIZE - 1) / TILE_SIZE, (g.extent.y + TILE_SIZE - 1) / TILE_SIZE, 1);
}

} // namespace

// ── Images (per-viewport state) ─────────────────────────────────────────────

void AoPass::Images::create(gpu::Device& gpu, uint32_t half_w, uint32_t half_h) {
    device = &gpu;
    auto made = gpu.create(gpu::TextureDesc{
        .format = gpu::Format::r8_unorm,
        .width = half_w,
        .height = half_h,
        .use = gpu::TextureUse::storage | gpu::TextureUse::sampled,
        .queues = gpu::Queues::graphics_and_compute,
        .initial = gpu::Clear{1.0F},
        .name = "ao",
    });
    if (!made) {
        FJELL_GFX_ERROR("{}", made.error());
        return;
    }
    ao = std::move(*made);
    extent = {half_w, half_h};
}

void AoPass::Images::destroy() {
    invalidate_imgui_texture();
    ao.reset();
    extent = {0, 0};
}

void AoPass::Images::resize(gpu::Device& gpu, uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;

    const uint32_t half_w = std::max(1U, width / 2);
    const uint32_t half_h = std::max(1U, height / 2);
    if (ao && extent.x == half_w && extent.y == half_h) return;

    // The editor's view of the old texture may still be in a frame's draw
    // data; it goes when that frame is done, and the next imgui_texture()
    // call makes one for the new texture.
    if (imgui_descriptor != VK_NULL_HANDLE) {
        gpu::vulkan::defer(gpu, [set = imgui_descriptor] { ImGui_ImplVulkan_RemoveTexture(set); });
        imgui_descriptor = VK_NULL_HANDLE;
    }
    create(gpu, half_w, half_h);
}

VkDescriptorSet AoPass::Images::imgui_texture(gpu::Sampler sampler) {
    if (imgui_descriptor == VK_NULL_HANDLE && ao) {
        imgui_descriptor = ImGui_ImplVulkan_AddTexture(
            gpu::vulkan::native_sampler(*device, sampler), gpu::vulkan::native_view(*device, ao),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return imgui_descriptor;
}

void AoPass::Images::invalidate_imgui_texture() {
    if (imgui_descriptor != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(imgui_descriptor);
        imgui_descriptor = VK_NULL_HANDLE;
    }
}

// ── AoPass ──────────────────────────────────────────────────────────────────

Result<> AoPass::create(Device& device, const PassResources& resources) {
    device_ = resources.device;
    ao_sampler_ = device_->sampler({.filter = gpu::Filter::linear,
                                    .mip_filter = gpu::Filter::nearest,
                                    .address = gpu::Address::clamp});
    FJELL_TRY(blur_pipeline_, device_->create(gpu::ComputePipelineDesc{
                                  .shader = "shaders/gtao_blur.comp",
                                  .shared = {resources.globals},
                                  .name = "gtao_blur",
                              }));

    // 1x1 placeholder so the global "ao_texture" binding is valid from frame 1.
    default_images_.create(*device_, 1, 1);

    if (auto r = create_trace(device, resources); !r) { return r; }

    FJELL_GFX_DEBUG("AO pass created");
    return {};
}

void AoPass::destroy() {
    destroy_trace();
    default_images_.destroy();
    blur_pipeline_.reset();
}

void AoPass::resize(uint32_t width, uint32_t height) {
    default_images_.resize(*device_, width, height);
}

VkDescriptorSet AoPass::imgui_texture() {
    return default_images_.imgui_texture(ao_sampler_);
}

void AoPass::invalidate_imgui_texture() {
    default_images_.invalidate_imgui_texture();
}

void AoPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    auto depth = builder.import_named(ctx, "depth");
    builder.read(depth, gpu::Access::sampled_compute);

    auto ao = builder.import_named(ctx, "gtao_ao");
    if (enabled_) {
        builder.write(ao, gpu::Access::storage_write_compute);
        declare_trace(builder, ctx);
    } else {
        // Disabled path: the AO is cleared, the only ao node running.
        builder.write(ao, gpu::Access::clear);
    }

    builder.has_side_effects();
}

void AoPass::record(const FrameContext& ctx) {
    auto* imgs = ctx.pp_resources ? ctx.pp_resources->gtao() : nullptr;
    auto& g = imgs != nullptr ? *imgs : default_images_;
    if (!g.ao) return;

    FJELL_PROFILE_SCOPE_N("record_gtao_trace");
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("GTAO Trace");

    if (!enabled_) {
        cmd.clear(g.ao, gpu::Clear{1.0F});   // no occlusion
        return;
    }

    record_trace(ctx, g);
}

void AoPass::collect_exports(DeclareContext& /*ctx*/) {
    // gtao_ao is imported by RenderPipeline::execute() from the active
    // pp_resources (per-viewport AO), not here — AoPass only knows about
    // its default placeholder Images, which isn't what any real viewport
    // writes to.
}

// ── GtaoTracePass ───────────────────────────────────────────────────────────

Result<> GtaoTracePass::create_trace(Device& /*device*/, const PassResources& resources) {
    FJELL_TRY(trace_pipeline_, device_->create(gpu::ComputePipelineDesc{
                                   .shader = "shaders/gtao.comp",
                                   .shared = {resources.globals},
                                   .name = "gtao",
                               }));
    return {};
}

void GtaoTracePass::destroy_trace() {
    trace_pipeline_.reset();
}

void GtaoTracePass::declare_trace(PassBuilder& /*builder*/, const DeclareContext& /*ctx*/) {
    // No additional accesses beyond AoPass::declare.
}

void GtaoTracePass::record_trace(const FrameContext& ctx, Images& g) {
    auto& cmd = ctx.commands();
    const auto target_extent = ctx.target->extent();
    cmd.set_pipeline(trace_pipeline_);
    cmd.bind(ctx.globals);
    cmd.bind({{"ao_output", gpu::storage(g.ao)}});
    cmd.push(GtaoPushConstants{
        .radius = radius(),
        .intensity = intensity(),
        .thickness = thickness(),
        .max_pixel_radius = 128.0F,
        .viewport_width = g.extent.x,
        .viewport_height = g.extent.y,
        .full_viewport_width = target_extent.width,
        .full_viewport_height = target_extent.height,
    });
    cmd.dispatch((g.extent.x + TILE_SIZE - 1) / TILE_SIZE, (g.extent.y + TILE_SIZE - 1) / TILE_SIZE, 1);
}

PassSetup GtaoTracePass::setup() const {
    return {
        .name = "ao_trace",
        .phase = RenderPhase::GpuDriven,
        .sub_order = 100,
        .async_hint = AsyncComputeHint::Prefer,
    };
}

// ── AoBlurHPass ─────────────────────────────────────────────────────────────

Result<> AoBlurHPass::create(Device& /*device*/, const PassResources& /*resources*/) {
    return {};
}

void AoBlurHPass::destroy() {}

void AoBlurHPass::post_create(GpuContext& /*gpu*/, PassRegistry& passes) {
    owner_ = &passes.get<AoPass>("ao_trace");
}

PassSetup AoBlurHPass::setup() const {
    return {
        .name = "ao_blur_h",
        .phase = RenderPhase::GpuDriven,
        .sub_order = 101,
        .async_hint = AsyncComputeHint::Prefer,
    };
}

void AoBlurHPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    if (owner_ == nullptr || !owner_->enabled()) { return; }

    auto ao = builder.import_named(ctx, "gtao_ao");
    builder.read(ao, gpu::Access::sampled_compute);

    auto normal = builder.import_named(ctx, "normal");
    builder.read(normal, gpu::Access::sampled_compute);

    TextureDesc blur_desc{};
    blur_desc.format = gpu::Format::r8_unorm;
    blur_desc.size_class = SizeClass::viewport;
    blur_desc.viewport_divisor = 2;
    auto blur = builder.create("gtao_blur", blur_desc);
    builder.write(blur, gpu::Access::storage_write_compute);
}

void AoBlurHPass::record(const FrameContext& ctx) {
    if (owner_ == nullptr || !owner_->enabled() || ctx.target == nullptr) { return; }

    auto* pp = ctx.pp_resources;
    auto* imgs = pp != nullptr ? pp->gtao() : nullptr;
    auto& g = imgs != nullptr ? *imgs : owner_->images();
    const gpu::Texture blur = ctx.texture("gtao_blur");
    if (!g.ao || !blur.valid()) { return; }

    FJELL_PROFILE_SCOPE_N("record_gtao_blur_h");
    auto zone = ctx.commands().zone("GTAO Blur H");
    record_blur(ctx, *owner_, g, blur, g.ao, 0);
}

// ── AoBlurVPass ─────────────────────────────────────────────────────────────

Result<> AoBlurVPass::create(Device& /*device*/, const PassResources& /*resources*/) {
    return {};
}

void AoBlurVPass::destroy() {}

void AoBlurVPass::post_create(GpuContext& /*gpu*/, PassRegistry& passes) {
    owner_ = &passes.get<AoPass>("ao_trace");
}

PassSetup AoBlurVPass::setup() const {
    return {
        .name = "ao_blur_v",
        .phase = RenderPhase::GpuDriven,
        .sub_order = 102,
        .async_hint = AsyncComputeHint::Prefer,
    };
}

void AoBlurVPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    if (owner_ == nullptr || !owner_->enabled()) { return; }

    // Declare the same virtual transient AoBlurHPass declared — the
    // graph dedupes virtual resources by name so both passes resolve to
    // the same TrackedImage. This lets the DAG sort see the blur_h →
    // blur_v edge and keeps blur_h alive through the pass-culling walk.
    TextureDesc blur_desc{};
    blur_desc.format = gpu::Format::r8_unorm;
    blur_desc.size_class = SizeClass::viewport;
    blur_desc.viewport_divisor = 2;
    auto blur = builder.create("gtao_blur", blur_desc);
    builder.read(blur, gpu::Access::sampled_compute);

    auto normal = builder.import_named(ctx, "normal");
    builder.read(normal, gpu::Access::sampled_compute);

    auto ao = builder.import_named(ctx, "gtao_ao");
    builder.write(ao, gpu::Access::storage_write_compute);
}

void AoBlurVPass::record(const FrameContext& ctx) {
    if (owner_ == nullptr || !owner_->enabled() || ctx.target == nullptr) { return; }

    auto* pp = ctx.pp_resources;
    auto* imgs = pp != nullptr ? pp->gtao() : nullptr;
    auto& g = imgs != nullptr ? *imgs : owner_->images();
    const gpu::Texture blur = ctx.texture("gtao_blur");
    if (!g.ao || !blur.valid()) { return; }

    FJELL_PROFILE_SCOPE_N("record_gtao_blur_v");
    auto zone = ctx.commands().zone("GTAO Blur V");
    record_blur(ctx, *owner_, g, g.ao, blur, 1);
}

} // namespace fjell
