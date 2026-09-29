#include "renderer/pass/sun_shafts_pass.hpp"

#include "core/log.hpp"
#include "core/profiler.hpp"
#include "environment/environment_state.hpp"
#include "environment/material_track.hpp"
#include "gpu/device.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass/water_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/pass_registry.hpp"

#include <algorithm>

namespace fjell {

struct ShaftsMarchPush {
    SunShaftsPass::RayPush ray;
    float density;
    float intensity;
    float anisotropy;
    uint32_t step_count;
    uint32_t frame_index;
    float directionality;
    float threshold;
    // When the camera is under a body of water the whole march is in it:
    // its medium's density replaces the haze's, unscaled, since water is
    // measured in the fog's unit already and its beams live within metres,
    // and its scattering is what the beams are worth. The haze's in-scatter
    // normalisation saturates at ten times the phase, which a medium this
    // dense reaches within metres: a wall of white rather than beams.
    float water_density;
    float water_scattering;
    uint32_t camera_below;
};

struct ShaftsResolvePush {
    SunShaftsPass::RayPush ray;
    uint32_t history_valid;
};

struct ShaftsCompositePush {
    glm::uvec2 viewport_size;
    glm::uvec2 shafts_size;
    float depth_tolerance;
};

namespace {

/// The march's output. Declared by the march that writes it and by the resolve
/// that reads it, and the two descriptions have to be the same one.
TextureDesc raw_desc() {
    TextureDesc desc{};
    desc.format = SunShaftsPass::RAW_FORMAT;
    desc.size_class = SizeClass::viewport;
    desc.viewport_divisor = SunShaftsPass::RESOLUTION_DIVISOR;
    return desc;
}

void dispatch(gpu::CommandList& cmd, glm::uvec2 extent) {
    cmd.dispatch((extent.x + SunShaftsPass::TILE_SIZE - 1) / SunShaftsPass::TILE_SIZE,
                 (extent.y + SunShaftsPass::TILE_SIZE - 1) / SunShaftsPass::TILE_SIZE, 1);
}

glm::uvec2 size_of(const OffscreenPass& target) {
    const auto extent = target.extent();
    return {extent.width, extent.height};
}

} // namespace

// ── Images ─────────────────────────────────────────────────────────────────

void SunShaftsPass::Images::create(gpu::Device& gpu, uint32_t screen_w, uint32_t screen_h) {
    const uint32_t w = std::max(1U, screen_w / RESOLUTION_DIVISOR);
    const uint32_t h = std::max(1U, screen_h / RESOLUTION_DIVISOR);

    extent = {w, h};
    ping_pong = 0;
    frame_counter = 0;
    history_valid = false;

    for (auto& image : shafts) {
        auto made = gpu.create(gpu::TextureDesc{
            .format = SHAFTS_FORMAT,
            .width = w,
            .height = h,
            .use = gpu::TextureUse::storage | gpu::TextureUse::sampled,
            .name = "sun_shafts",
        });
        if (!made) {
            FJELL_GFX_ERROR("{}", made.error());
            destroy();
            return;
        }
        image = std::move(*made);
    }
}

void SunShaftsPass::Images::destroy() {
    for (auto& image : shafts) image.reset();
    extent = {0, 0};
    history_valid = false;
}

void SunShaftsPass::Images::resize(gpu::Device& gpu, uint32_t screen_w, uint32_t screen_h) {
    const uint32_t w = std::max(1U, screen_w / RESOLUTION_DIVISOR);
    const uint32_t h = std::max(1U, screen_h / RESOLUTION_DIVISOR);
    if (w == extent.x && h == extent.y) return;
    destroy();
    create(gpu, screen_w, screen_h);
}

// ── SunShaftsPass ──────────────────────────────────────────────────────────

Result<> SunShaftsPass::create(Device& /*device*/, const PassResources& resources) {
    device_ = resources.device;
    sampler_ = device_->sampler({.filter = gpu::Filter::linear,
                                 .mip_filter = gpu::Filter::nearest,
                                 .address = gpu::Address::clamp});
    auto build = [&](const char* shader, const char* name) {
        return device_->create(gpu::ComputePipelineDesc{
            .shader = shader,
            .shared = {resources.globals},
            .name = name,
        });
    };
    FJELL_TRY(march_pipeline_, build("shaders/sun_shafts.comp", "sun_shafts"));
    FJELL_TRY(resolve_pipeline_, build("shaders/sun_shafts_resolve.comp", "sun_shafts_resolve"));
    FJELL_TRY(composite_pipeline_,
              build("shaders/sun_shafts_composite.comp", "sun_shafts_composite"));

    default_images_.create(*device_, 2, 2);

    FJELL_GFX_DEBUG("Sun shafts pass created");
    return {};
}

void SunShaftsPass::destroy() {
    default_images_.destroy();
    march_pipeline_.reset();
    resolve_pipeline_.reset();
    composite_pipeline_.reset();
}

void SunShaftsPass::prepare_frame(const FrameContext& ctx, GpuContext& /*gpu*/) {
    enabled_ = false;
    if (ctx.environment == nullptr) return;

    const auto& shafts = ctx.environment->sun_shafts;
    enabled_ = ctx.environment->fog.enabled && shafts.enabled;
    // Shafts come from the leading body's direction alone, and that body
    // changes between two frames at dusk and dawn. They thin out toward the
    // change instead, so it has nothing of theirs to cut.
    intensity_ = shafts.intensity * ctx.environment->primary_lead;
    density_mult_ = shafts.density_mult;
    max_reach_ = shafts.max_reach;
    step_count_ = shafts.step_count;
    anisotropy_ = shafts.anisotropy;
    directionality_ = shafts.directionality;
    threshold_ = shafts.threshold;
    // The march reads one scalar density: the fog material's `density`
    // uniform where its shader declares one, else nothing to march through.
    fog_density_ = ctx.environment->fog.scalar(material_track::FOG_DENSITY_UNIFORM, 0.0F);

    // Each water medium's density, read the way the haze's is: a medium
    // whose shader declares none has nothing for the sun to scatter from.
    water_march_.clear();
    for (const WaterMedium& water : ctx.environment->water_media) {
        WaterMarch march;
        bool has_density = false;
        for (const NamedValue& value : water.uniforms) {
            if (value.name == material_track::FOG_DENSITY_UNIFORM) {
                march.density = value.values[0];
                has_density = true;
            } else if (value.name == "scattering") {
                march.scattering = value.values[0];
            }
        }
        if (has_density) water_march_[water.node_id] = march;
    }
}

PassSetup SunShaftsPass::setup() const {
    return {
        .name = "sun_shafts",
        .phase = RenderPhase::PostProcess,
        .sub_order = 120,
    };
}

SunShaftsPass::Images& SunShaftsPass::frame_images(glm::uvec2 viewport) {
    default_images_.resize(*device_, viewport.x, viewport.y);
    return default_images_;
}

SunShaftsPass::RayPush SunShaftsPass::ray_push(const Images& imgs, glm::uvec2 viewport) const {
    return {.output_size = imgs.extent, .full_size = viewport, .max_reach = max_reach_};
}

void SunShaftsPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    if (!enabled_) { return; }

    auto depth = builder.import_named(ctx, DEPTH_NAME);
    builder.read(depth, gpu::Access::sampled_compute);
    // The march samples the cascades every step, so the shadow pass has to
    // finish first. Declared rather than left to the producer's inline
    // finalize barrier, which is the edge the DAG could not otherwise see.
    if (auto atlas = builder.import_named(ctx, "shadow_atlas"); atlas.valid()) {
        builder.read(atlas, gpu::Access::sampled_compute);
    }
    // Clouds shade the shafts too.
    if (auto clouds = builder.import_named_optional(ctx, "cloud_shadow"); clouds.valid()) {
        builder.read(clouds, gpu::Access::sampled_compute);
    }

    auto raw = builder.create(RAW_NAME, raw_desc());
    builder.write(raw, gpu::Access::storage_write_compute);
}

void SunShaftsPass::record(const FrameContext& ctx) {
    if (!enabled_ || ctx.target == nullptr) return;
    const gpu::Texture raw = ctx.texture(RAW_NAME);
    const gpu::Texture surface_depth = ctx.texture(DEPTH_NAME);
    if (!raw.valid() || !surface_depth.valid()) return;

    FJELL_PROFILE_SCOPE_N("record_sun_shafts");
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Sun Shafts");

    const auto& imgs = default_images_;
    ShaftsMarchPush push{};
    push.ray = ray_push(imgs, size_of(*ctx.target));
    // Effective density for shafts. The fog's base density is tuned for a
    // 3D froxel volume that accumulates over 64+ slices; a screen-space ray
    // march needs a larger coefficient to produce visible in-scatter at the
    // same apparent thickness.
    constexpr float SHAFT_DENSITY_SCALE = 10.0f;
    push.density = std::max(fog_density_ * density_mult_ * SHAFT_DENSITY_SCALE, 1e-4f);
    push.intensity = intensity_;
    push.anisotropy = std::clamp(anisotropy_, 0.0f, 0.99f);
    push.step_count = static_cast<uint32_t>(std::max(step_count_, 4));
    // Wrapped, because the shader turns it into a float and multiplies it up
    // for its dither: left to grow, the product loses its fraction within the
    // hour and the dither stops changing from frame to frame. 64 is the period
    // interleaved gradient noise is built to cycle over.
    push.frame_index = imgs.frame_counter % 64;
    push.directionality = std::clamp(directionality_, 0.0f, 1.0f);
    push.threshold = std::clamp(threshold_, 0.0f, 0.95f);
    // Under a body of water with a medium, the march is through that water.
    // The water pass decided the camera's side this frame.
    push.water_density = 0.0f;
    push.water_scattering = 0.0f;
    push.camera_below = 0;
    if (!water_march_.empty() && ctx.passes != nullptr) {
        const auto& water_pass = ctx.passes->get<WaterPass>("water");
        for (const auto& [node_id, march] : water_march_) {
            if (water_pass.camera_below(node_id)) {
                push.water_density = std::max(march.density, 1e-4f);
                push.water_scattering = march.scattering * density_mult_;
                push.camera_below = 1;
                break;
            }
        }
    }

    cmd.set_pipeline(march_pipeline_);
    cmd.bind(ctx.globals);
    cmd.bind({{"shafts_raw", gpu::storage(raw)},
              {"depth_texture", gpu::sampled(surface_depth, sampler_)}});
    cmd.push(push);
    dispatch(cmd, imgs.extent);
}

// ── SunShaftsResolvePass ───────────────────────────────────────────────────

Result<> SunShaftsResolvePass::create(Device& /*device*/, const PassResources& /*resources*/) {
    return {};
}

void SunShaftsResolvePass::destroy() {}

void SunShaftsResolvePass::post_create(GpuContext& /*gpu*/, PassRegistry& passes) {
    core_ = &passes.get<SunShaftsPass>("sun_shafts");
}

PassSetup SunShaftsResolvePass::setup() const {
    return {
        .name = "sun_shafts_resolve",
        .phase = RenderPhase::PostProcess,
        .sub_order = 121,
    };
}

void SunShaftsResolvePass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    if (core_ == nullptr || !core_->enabled()) { return; }

    auto raw = builder.create(SunShaftsPass::RAW_NAME, raw_desc());
    auto cur = builder.import_named(ctx, SunShaftsPass::HISTORY_CUR_NAME);
    auto prev = builder.import_named(ctx, SunShaftsPass::HISTORY_PREV_NAME);
    auto depth = builder.import_named(ctx, SunShaftsPass::DEPTH_NAME);
    // The resolved velocity: under MSAA the multisampled one cannot be sampled.
    auto velocity = builder.import_named(ctx, "velocity");
    builder.read(raw, gpu::Access::sampled_compute);
    builder.read(prev, gpu::Access::sampled_compute);
    builder.read(depth, gpu::Access::sampled_compute);
    builder.read(velocity, gpu::Access::sampled_compute);
    builder.write(cur, gpu::Access::storage_write_compute);
}

void SunShaftsResolvePass::record(const FrameContext& ctx) {
    if (core_ == nullptr || !core_->enabled() || ctx.target == nullptr) { return; }
    const gpu::Texture raw = ctx.texture(SunShaftsPass::RAW_NAME);
    const gpu::Texture surface_depth = ctx.texture(SunShaftsPass::DEPTH_NAME);
    if (!raw.valid() || !surface_depth.valid()) { return; }

    FJELL_PROFILE_SCOPE_N("record_sun_shafts_resolve");
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Sun Shafts Resolve");

    auto& imgs = core_->images();
    const gpu::Sampler sampler = core_->sampler();
    cmd.set_pipeline(core_->resolve_pipeline());
    cmd.bind(ctx.globals);
    cmd.bind({{"shafts_out", gpu::storage(imgs.shafts[imgs.cur_index()])},
              {"shafts_raw", gpu::sampled(raw, sampler)},
              {"shafts_history", gpu::sampled(imgs.shafts[imgs.prev_index()], sampler)},
              {"depth_texture", gpu::sampled(surface_depth, sampler)},
              {"velocity_texture", gpu::sampled(ctx.target->velocity_texture(), sampler)}});
    cmd.push(ShaftsResolvePush{
        .ray = core_->ray_push(imgs, size_of(*ctx.target)),
        .history_valid = imgs.history_valid ? 1U : 0U,
    });
    dispatch(cmd, imgs.extent);
}

// ── SunShaftsCompositePass ─────────────────────────────────────────────────

Result<> SunShaftsCompositePass::create(Device& /*device*/, const PassResources& /*resources*/) {
    return {};
}

void SunShaftsCompositePass::destroy() {}

void SunShaftsCompositePass::post_create(GpuContext& /*gpu*/, PassRegistry& passes) {
    core_ = &passes.get<SunShaftsPass>("sun_shafts");
}

PassSetup SunShaftsCompositePass::setup() const {
    return {
        .name = "sun_shafts_composite",
        .phase = RenderPhase::PostProcess,
        .sub_order = 122,
    };
}

void SunShaftsCompositePass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    if (core_ == nullptr || !core_->enabled()) { return; }

    auto shafts = builder.import_named(ctx, SunShaftsPass::HISTORY_CUR_NAME);
    auto depth = builder.import_named(ctx, SunShaftsPass::DEPTH_NAME);
    auto color = builder.import_named(ctx, "color");
    builder.read(shafts, gpu::Access::sampled_compute);
    builder.read(depth, gpu::Access::sampled_compute);
    builder.read_write(color, gpu::Access::storage_read_write_compute);
}

void SunShaftsCompositePass::record(const FrameContext& ctx) {
    if (core_ == nullptr || !core_->enabled() || ctx.target == nullptr) { return; }
    const gpu::Texture surface_depth = ctx.texture(SunShaftsPass::DEPTH_NAME);
    if (!surface_depth.valid()) { return; }

    FJELL_PROFILE_SCOPE_N("record_sun_shafts_composite");
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Sun Shafts Composite");

    auto& imgs = core_->images();
    const glm::uvec2 extent = size_of(*ctx.target);
    const gpu::Sampler sampler = core_->sampler();
    cmd.set_pipeline(core_->composite_pipeline());
    cmd.bind(ctx.globals);
    cmd.bind({{"shafts_tex", gpu::sampled(imgs.shafts[imgs.cur_index()], sampler)},
              {"depth_texture", gpu::sampled(surface_depth, sampler)},
              {"scene_color", gpu::storage(ctx.target->color_texture())}});
    cmd.push(ShaftsCompositePush{
        .viewport_size = extent,
        .shafts_size = imgs.extent,
        // Clamped above zero: a zero tolerance rejects every tap including the
        // pixel's own, and the upsample would fall back to nearest everywhere.
        .depth_tolerance = std::max(core_->upsample_depth_tolerance(), 1e-4F),
    });
    dispatch(cmd, extent);

    // What was resolved this frame is next frame's history: the images swap
    // roles rather than being copied.
    imgs.history_valid = true;
    imgs.ping_pong = imgs.prev_index();
    ++imgs.frame_counter;
}

} // namespace fjell
