#include "renderer/pass/contact_shadow_pass.hpp"

#include "core/log.hpp"
#include "core/profiler.hpp"
#include "environment/environment_state.hpp"
#include "gpu/device.hpp"
#include "renderer/global_bindings.hpp"
#include "renderer/gpu_context.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass_builder.hpp"

#include <algorithm>

namespace fjell {

namespace {

struct ContactShadowPush {
    glm::uvec2 viewport_size;
    float max_distance;
    float thickness;
    uint32_t step_count;
    float frame_jitter;
    float pad0;
    float pad1;
};

} // namespace

Result<> ContactShadowPass::create(const PassResources& resources) {
    device_ = resources.device;
    // How the surfaces read the mask through the global set.
    sampler_ = device_->sampler({.filter = gpu::Filter::linear,
                                 .mip_filter = gpu::Filter::nearest,
                                 .address = gpu::Address::clamp});
    FJELL_TRY(pipeline_, device_->create(gpu::ComputePipelineDesc{
                             .shader = "shaders/contact_shadow.comp",
                             .shared = {resources.globals},
                             .name = "contact_shadow",
                         }));

    // Start at a common desktop size and grow from ensure_size() as viewports
    // demand it. The compute shader writes only the region named by
    // viewport_size in its push constants, so an image larger than the
    // viewport is fine; one smaller is not, because the stores outside it are
    // discarded and shading then reads texels that were never written.
    // Growing happens between frames, never during record(), which is what
    // made the earlier resize-on-record approach a descriptor-invalidation
    // storm.
    if (auto made = make_shadow(1920, 1080); !made) return made;

    FJELL_GFX_DEBUG("Contact shadow pass created");
    return {};
}

void ContactShadowPass::post_create(GpuContext& gpu, PassRegistry& /*passes*/) {
    // Every frame's global binding points at the mask from the very first
    // draw.
    gpu.global_bindings().set("contact_shadow", gpu::sampled(shadow_, sampler_));
}

void ContactShadowPass::destroy() {
    pipeline_.reset();
    shadow_.reset();
}

Result<> ContactShadowPass::make_shadow(uint32_t w, uint32_t h) {
    FJELL_TRY(shadow_, device_->create(gpu::TextureDesc{
                           .format = gpu::Format::r8_unorm,
                           .width = w,
                           .height = h,
                           .use = gpu::TextureUse::storage | gpu::TextureUse::sampled,
                           .initial = gpu::Clear{1.0f},
                           .name = "contact_shadow",
                       }));
    extent_ = {w, h};
    return {};
}

void ContactShadowPass::ensure_size(uint32_t w, uint32_t h) {
    if (w <= extent_.x && h <= extent_.y) return;
    // One image serves every viewport, so grow to cover the largest rather
    // than thrashing between sizes as different panels ask for different ones.
    if (auto made = make_shadow(std::max(w, extent_.x), std::max(h, extent_.y)); !made) {
        FJELL_GFX_ERROR("{}", made.error());
    }
}

void ContactShadowPass::prepare_frame(const FrameContext& ctx, GpuContext& /*gpu*/) {
    if (ctx.environment == nullptr) return;
    const auto& cs = ctx.environment->contact_shadows;
    enabled_ = cs.enabled;
    max_distance_ = cs.distance;
    thickness_ = cs.thickness;
    step_count_ = cs.steps;
}

PassSetup ContactShadowPass::setup() const {
    return {
        .name = "contact_shadow",
        // Traces the prepass depth, so it runs once that is resolved and
        // before the colour passes whose surfaces sample the result.
        .phase = RenderPhase::GpuDriven,
        .sub_order = 105,   // after GTAO (100-102)
    };
}

void ContactShadowPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    auto shadow = builder.import_named(ctx, "contact_shadow");
    if (!enabled_) {
        builder.write(shadow, gpu::Access::clear);
        return;
    }
    builder.read(builder.import_named(ctx, "depth"), gpu::Access::sampled_compute);
    builder.write(shadow, gpu::Access::storage_write_compute);
}

void ContactShadowPass::collect_exports(DeclareContext& ctx) {
    if (!shadow_) { return; }
    // The surfaces sample it in the fragment shader through the global set.
    ctx.export_texture("contact_shadow", shadow_, {.resting = gpu::Access::sampled_fragment});
}

void ContactShadowPass::record(const FrameContext& ctx) {
    if (ctx.target == nullptr) return;

    FJELL_PROFILE_SCOPE_N("record_contact_shadow");
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Contact Shadow");

    // Disabled: hold 1.0, no occlusion, for the surfaces that still sample
    // it.
    if (!enabled_) {
        cmd.clear(shadow_, gpu::Clear{1.0f});
        ++frame_counter_;
        return;
    }

    const auto extent = ctx.target->extent();
    cmd.set_pipeline(pipeline_);
    cmd.bind(ctx.globals);
    cmd.bind({{"contact_out", gpu::storage(shadow_)}});
    cmd.push(ContactShadowPush{
        .viewport_size = glm::uvec2(extent.x, extent.y),
        .max_distance = max_distance_,
        .thickness = thickness_,
        .step_count = static_cast<uint32_t>(std::max(step_count_, 4)),
        .frame_jitter = static_cast<float>(frame_counter_ % 64) * (1.0f / 64.0f),
        .pad0 = 0.0f,
        .pad1 = 0.0f,
    });
    const uint32_t groups_x = (extent.x + TILE_SIZE - 1) / TILE_SIZE;
    const uint32_t groups_y = (extent.y + TILE_SIZE - 1) / TILE_SIZE;
    cmd.dispatch(groups_x, groups_y, 1);
    ++frame_counter_;
}

} // namespace fjell
