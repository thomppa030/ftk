#include "renderer/pass/sky_cubemap_pass.hpp"

#include "core/base_path.hpp"
#include "core/project.hpp"
#include "environment/environment_state.hpp"
#include "ftk/base/log.hpp"
#include "ftk/base/profiler.hpp"
#include "ftk/framegraph/pass_builder.hpp"
#include "ftk/framegraph/resource_registry.hpp"
#include "ftk/gpu/device.hpp"
#include "ftk/gpu/shader.hpp"
#include "ftk/gpu/upload.hpp"
#include "renderer/global_bindings.hpp"
#include "renderer/gpu_context.hpp"
#include "renderer/pass/sky_pass.hpp"
#include "renderer/pass_registry.hpp"
#include "renderer/pipeline_cache.hpp"

#include <stb_image.h>

#include <algorithm>
#include <memory>
#include <span>

namespace fjell {

namespace {

// One mip of a cubemap as its six faces, the layers a compute shader writes.
ftk::gpu::TextureView faces(ftk::gpu::Texture cubemap, uint32_t mip) {
    ftk::gpu::TextureView view = ftk::gpu::mip(cubemap, mip);
    view.kind = ftk::gpu::ViewKind::tex2d_array;
    return view;
}

// Workgroups covering a face of `size` texels, eight by eight.
uint32_t groups(uint32_t size) {
    return (size + 7) / 8;
}

} // namespace

ftk::Result<> SkyCubemapPass::create(const PassResources& resources) {
    device_ = resources.device;
    resources_ = resources;
    ftk::gpu::Device& device = *device_;

    // Cleared until generated in the first frame this pass runs, so anything
    // sampling them before reads something defined.
    FTK_TRY(cubemap_, device.create(ftk::gpu::TextureDesc{
                            .kind = ftk::gpu::TextureKind::cube,
                            .format = ftk::gpu::Format::rgba16_float,
                            .width = CUBEMAP_SIZE,
                            .height = CUBEMAP_SIZE,
                            .mips = MIP_LEVELS,
                            .use = ftk::gpu::TextureUse::storage | ftk::gpu::TextureUse::sampled,
                            .initial = ftk::gpu::Clear{0.0f, 0.0f, 0.0f, 1.0f},
                            .name = "sky_cubemap",
                        }));
    FTK_TRY(brdf_lut_, device.create(ftk::gpu::TextureDesc{
                             .format = ftk::gpu::Format::rg16_float,
                             .width = BRDF_LUT_SIZE,
                             .height = BRDF_LUT_SIZE,
                             .use = ftk::gpu::TextureUse::storage | ftk::gpu::TextureUse::sampled,
                             .initial = ftk::gpu::Clear{0.0f, 0.0f, 0.0f, 0.0f},
                             .name = "brdf_lut",
                         }));
    cubemap_sampler_ = device.sampler({.filter = ftk::gpu::Filter::linear,
                                       .mip_filter = ftk::gpu::Filter::linear,
                                       .address = ftk::gpu::Address::clamp});
    brdf_lut_sampler_ = device.sampler({.filter = ftk::gpu::Filter::linear,
                                        .mip_filter = ftk::gpu::Filter::nearest,
                                        .address = ftk::gpu::Address::clamp});

    FTK_TRY(cubemap_pipeline_, device.create(ftk::gpu::ComputePipelineDesc{
                                     .shader = "shaders/sky_cubemap.comp",
                                     .name = "sky_cubemap",
                                 }));
    FTK_TRY(brdf_pipeline_, device.create(ftk::gpu::ComputePipelineDesc{
                                  .shader = "shaders/brdf_lut.comp",
                                  .name = "brdf_lut",
                              }));
    FTK_TRY(equirect_pipeline_, device.create(ftk::gpu::ComputePipelineDesc{
                                      .shader = "shaders/equirect_to_cube.comp",
                                      .name = "equirect_to_cube",
                                  }));
    FTK_TRY(prefilter_pipeline_, device.create(ftk::gpu::ComputePipelineDesc{
                                       .shader = "shaders/prefilter_envmap.comp",
                                       .name = "prefilter_envmap",
                                   }));

    FTK_GFX_DEBUG("Sky cubemap pass created");
    return {};
}

void SkyCubemapPass::post_create(GpuContext& gpu, PassRegistry& passes) {
    // Copy initial sky colors from the sky gradient pass
    auto& sky = passes.get<SkyPass>("sky");
    sky_pass_ = &sky;
    set_colors(sky.top_color(), sky.bottom_color());

    bind_sky(gpu);
    gpu.global_bindings().set("brdf_lut", ftk::gpu::sampled(brdf_lut_, brdf_lut_sampler_));
}

void SkyCubemapPass::register_resources(ftk::ResourceRegistry& registry) {
    registry_ = &registry;
    const ftk::gpu::Texture sky = sky_texture();
    registry.register_image("sky_cubemap", {.frame_graph_id = 0, .texture = sky});
}

void SkyCubemapPass::destroy() {
    fjsl_pipeline_.reset();
    cubemap_pipeline_.reset();
    brdf_pipeline_.reset();
    equirect_pipeline_.reset();
    prefilter_pipeline_.reset();
    hdri_cubemap_.reset();
    hdri_equirect_.reset();
    hdri_source_.reset();
    cubemap_.reset();
    brdf_lut_.reset();
}

void SkyCubemapPass::bind_sky(GpuContext& gpu) {
    // Every frame slot's global set takes the new sky; a frame still reading
    // the old one keeps the set it bound, and previews take it when they are
    // next refreshed.
    gpu.global_bindings().set("sky_cubemap", ftk::gpu::sampled(sky_texture(), cubemap_sampler_));
}

void SkyCubemapPass::prepare_frame(const FrameContext& ctx, GpuContext& gpu) {
    if (ctx.environment == nullptr) return;

    // The shadow-casting body lights the atmospheric cubemap, so IBL matches
    // the sky pass and the direct lighting term.
    const auto& sun = ctx.environment->primary;
    const auto& sky = ctx.environment->sky;
    // An FJSL sky renders the cubemap itself and takes over from an HDRI;
    // its inputs are what decide whether the cubemap is stale.
    fjsl_active_ = !sky.sky_shader.empty();
    if (fjsl_active_) {
        FjslInputs inputs;
        inputs.ubo = make_environment_ubo(*ctx.environment);
        inputs.uniforms = ctx.environment->sky_uniforms;
        inputs.generation = sky_pass_ != nullptr ? sky_pass_->fjsl_pipeline_generation() : 0;
        if (!fjsl_rendered_ || !(inputs == fjsl_inputs_)) {
            fjsl_inputs_ = std::move(inputs);
            dirty_ = true;
        }
    } else if (fjsl_rendered_) {
        // Back to a built-in sky: the cubemap holds the FJSL result.
        fjsl_rendered_ = false;
        dirty_ = true;
    }
    // Build (or rebuild after the sky shader changed) the cubemap variant
    // here, before anything records, rather than in the middle of record().
    if (fjsl_active_ && sky_pass_ != nullptr && sky_pass_->fjsl_active()) {
        ensure_fjsl_pipeline();
    }
    set_colors(sky.top_color, sky.bottom_color);
    set_atmosphere(sky.atmosphere_enabled, sun.direction, sun.color,
                   sky.atmosphere_sun_intensity,
                   sky.atmosphere_rayleigh_scale,
                   sky.atmosphere_mie_scale,
                   sky.atmosphere_mie_g,
                   sky.atmosphere_mie_coeff);

    // Load HDRI if path changed (works regardless of atmosphere mode —
    // atmosphere modulates the HDRI color rather than replacing it).
    if (!fjsl_active_ && !sky.hdri_path.empty() && sky.hdri_path != loaded_hdri_path_) {
        load_hdri(base_path::resolve_asset(sky.hdri_path, project_));
        // Not read again while the path stays, whether or not it loaded.
        loaded_hdri_path_ = sky.hdri_path;
        if (hdri_pending_) bind_sky(gpu);
    } else if ((sky.hdri_path.empty() || fjsl_active_) && !loaded_hdri_path_.empty()) {
        // The HDRI goes and the procedural cubemap it covered is rendered
        // again. The frames still reading the HDRI keep it until they are
        // done.
        loaded_hdri_path_.clear();
        const bool had_hdri = has_hdri();
        hdri_cubemap_.reset();
        hdri_equirect_.reset();
        hdri_source_.reset();
        hdri_pending_ = false;
        if (had_hdri) bind_sky(gpu);
        dirty_ = true;
    }
}

void SkyCubemapPass::set_colors(const glm::vec3& top, const glm::vec3& bottom) {
    if (top_color_ != top || bottom_color_ != bottom) {
        top_color_ = top;
        bottom_color_ = bottom;
        dirty_ = true;
    }
}

void SkyCubemapPass::set_atmosphere(bool enabled, const glm::vec3& sun_dir,
                                     const glm::vec3& sun_color, float sun_intensity,
                                     float rayleigh_scale, float mie_scale,
                                     float mie_g, float mie_coeff) {
    bool changed = enabled != atmosphere_enabled_;
    if (enabled) {
        if (sun_dir != atm_sun_dir_ || sun_color != atm_sun_color_ ||
            sun_intensity != atm_sun_intensity_ ||
            rayleigh_scale != atm_rayleigh_scale_ ||
            mie_scale != atm_mie_scale_ || mie_g != atm_mie_g_ ||
            mie_coeff != atm_mie_coeff_) {
            changed = true;
        }
    }
    atmosphere_enabled_ = enabled;
    atm_sun_dir_ = sun_dir;
    atm_sun_color_ = sun_color;
    atm_sun_intensity_ = sun_intensity;
    atm_rayleigh_scale_ = rayleigh_scale;
    atm_mie_scale_ = mie_scale;
    atm_mie_g_ = mie_g;
    atm_mie_coeff_ = mie_coeff;
    if (changed) dirty_ = true;
}

void SkyCubemapPass::record(const FrameContext& ctx) {
    FTK_PROFILE_SCOPE_N("record_sky_cubemap");
    // Everything that decides what is written was read in prepare_frame(),
    // before declare() told the graph about this frame's writes; nothing may
    // change it here or a write would go unbarriered.
    const bool brdf = !brdf_generated_;
    const bool hdri = hdri_pending_;
    const bool sky = generates_this_frame();
    if (!brdf && !hdri && !sky) return;

    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Sky Cubemap");
    if (brdf) generate_brdf_lut(cmd);
    if (hdri) convert_hdri(cmd);
    if (sky) {
        if (fjsl_active_ && sky_pass_ != nullptr && sky_pass_->fjsl_active()) {
            generate_fjsl(ctx);
        } else {
            generate(cmd);
        }
    }
}

void SkyCubemapPass::generate_brdf_lut(ftk::gpu::CommandList& cmd) {
    cmd.set_pipeline(brdf_pipeline_);
    cmd.bind({{"lut", ftk::gpu::storage(brdf_lut_)}});
    cmd.dispatch(groups(BRDF_LUT_SIZE), groups(BRDF_LUT_SIZE), 1);
    brdf_generated_ = true;
    FTK_GFX_DEBUG("BRDF LUT generated ({}x{})", BRDF_LUT_SIZE, BRDF_LUT_SIZE);
}

void SkyCubemapPass::generate(ftk::gpu::CommandList& cmd) {
    struct PushConstants {
        glm::vec4 top_color;
        glm::vec4 bottom_color;
        glm::vec4 light_direction;  // xyz = sun dir, w unused
        glm::vec4 light_color;      // xyz = rgb, w unused
        uint32_t face_size;
        uint32_t sky_mode;          // 0 = gradient, 2 = atmosphere
        float atmosphere_rayleigh_scale;
        float atmosphere_mie_scale;
        float atmosphere_mie_g;
        float atmosphere_sun_intensity;
        float atmosphere_mie_coeff;
        float padding0;
    } push{
        glm::vec4(top_color_, 1.0f),
        glm::vec4(bottom_color_, 1.0f),
        glm::vec4(atm_sun_dir_, 0.0f),
        glm::vec4(atm_sun_color_, 0.0f),
        CUBEMAP_SIZE,
        atmosphere_enabled_ ? 2u : 0u,
        atm_rayleigh_scale_,
        atm_mie_scale_,
        atm_mie_g_,
        atm_sun_intensity_,
        atm_mie_coeff_,
        0.0f
    };

    cmd.set_pipeline(cubemap_pipeline_);
    cmd.bind({{"cubemap", ftk::gpu::storage(faces(cubemap_, 0))}});
    cmd.push(push);
    cmd.dispatch(groups(CUBEMAP_SIZE), groups(CUBEMAP_SIZE), 6);
    finish_cubemap(cmd);
    dirty_ = false;
}

void SkyCubemapPass::finish_cubemap(ftk::gpu::CommandList& cmd) {
    // Mip 0 as the compute shader wrote it, the rest to be filled from it:
    // all as copy targets, which is where the chain starts and ends.
    cmd.barrier(cubemap_, ftk::gpu::Access::storage_write_compute, ftk::gpu::Access::copy_dst);
    cmd.generate_mipmaps(cubemap_);
}

void SkyCubemapPass::ensure_fjsl_pipeline() {
    const uint32_t pid = sky_pass_->fjsl_pipeline_id();
    const uint32_t generation = sky_pass_->fjsl_pipeline_generation();
    // Tried once per compile of the shader, whether it builds or not.
    if (fjsl_pipeline_id_ == pid && fjsl_generation_ == generation) return;
    fjsl_pipeline_id_ = pid;
    fjsl_generation_ = generation;
    // A frame in flight may have rendered the cubemap with the old one; the
    // device releases it once that frame is done.
    fjsl_pipeline_.reset();

    auto* cache = sky_pass_->pipeline_cache();
    if (cache == nullptr) return;
    const std::string& spv = cache->sky_cubemap_spv(pid);
    if (spv.empty()) return;
    auto code = ftk::gpu::read_spirv(spv);
    if (!code) {
        FTK_GFX_ERROR("Sky cubemap: {}", code.error());
        return;
    }
    // Set 0 the globals, set 1 the bindless material set, set 2 the faces.
    auto made = device_->create(ftk::gpu::ComputePipelineDesc{
        .shader = std::span<const uint32_t>(*code),
        .shared = {resources_.globals, resources_.bindless},
        .name = "sky_cubemap_fjsl",
    });
    if (!made) {
        FTK_GFX_ERROR("Sky cubemap: {}", made.error());
        return;
    }
    fjsl_pipeline_ = std::move(*made);
}

void SkyCubemapPass::generate_fjsl(const FrameContext& ctx) {
    auto& cmd = ctx.commands();
    if (!fjsl_pipeline_ || sky_pass_->fjsl_material_id() == ~0u) {
        // No cubemap variant compiled, or the sky has not taken a material
        // row yet. Either way the built-in path keeps the cubemap valid
        // rather than leaving it stale — and reading a row that does not
        // exist would sample past the end of the material buffer.
        generate(cmd);
        return;
    }

    struct PushConstants {
        float viewport_width;
        float viewport_height;
        uint32_t sky_material_id;
        uint32_t face_size;
    } push{0.0f, 0.0f, sky_pass_->fjsl_material_id(), CUBEMAP_SIZE};

    cmd.set_pipeline(fjsl_pipeline_);
    cmd.bind(ctx.globals);
    cmd.bind(ctx.bindless);
    cmd.bind({{"sky_cube", ftk::gpu::storage(faces(cubemap_, 0))}});
    cmd.push(push);
    cmd.dispatch(groups(CUBEMAP_SIZE), groups(CUBEMAP_SIZE), 6);
    finish_cubemap(cmd);
    dirty_ = false;
    fjsl_rendered_ = true;
}

PassSetup SkyCubemapPass::setup() const {
    return {
        .name = "sky_cubemap",
        .phase = RenderPhase::Environment,
        .sub_order = 50,
    };
}

void SkyCubemapPass::declare(ftk::PassBuilder& builder, const DeclareContext& ctx) {
    // Each write is declared only in the frame that makes it; in any other
    // the pass declares nothing and the graph culls it. The declared write
    // brings the whole texture to storage for the compute shaders.
    if (!brdf_generated_) {
        if (auto lut = builder.import_named(ctx, "brdf_lut"); lut.valid()) {
            builder.write(lut, ftk::gpu::Access::storage_write_compute);
            // Surfaces sample it through the global set, which the graph
            // does not see.
            builder.has_side_effects();
        }
    }
    if (hdri_pending_ || generates_this_frame()) {
        auto cubemap = builder.import_named(ctx, "sky_cubemap");
        if (!cubemap.valid()) return;
        builder.write(cubemap, ftk::gpu::Access::storage_write_compute);
        // The procedural cubemap's mips are filled by copies from the first,
        // after the compute write; the HDRI's are all written as storage.
        if (!hdri_pending_) builder.leaves(cubemap, ftk::gpu::Access::copy_dst, ftk::gpu::Access::copy_dst);
    }
}

void SkyCubemapPass::collect_exports(DeclareContext& ctx) {
    // Both rest where the sky and every material sample them through the
    // global set: a reader can come first, as it does on every frame the
    // pass does not write them.
    const ftk::TextureExport sampled{.persistent = true, .resting = ftk::gpu::Access::sampled_fragment};
    ctx.export_texture("sky_cubemap", sky_texture(), sampled);
    ctx.export_texture("brdf_lut", brdf_lut_, sampled);
}

// ── HDRI environment map support ─────────────────────────────────────────

void SkyCubemapPass::load_hdri(const std::string& path) {
    int width = 0;
    int height = 0;
    int channels = 0;
    const std::unique_ptr<float, decltype(&stbi_image_free)> pixels(
        stbi_loadf(path.c_str(), &width, &height, &channels, 4), stbi_image_free);
    if (!pixels) {
        FTK_GFX_ERROR("Failed to load HDRI: {}", path);
        return;
    }
    FTK_GFX_INFO("Loaded HDRI: {}x{} from {}", width, height, path);

    ftk::gpu::Device& device = *device_;
    const auto made = [&](const ftk::gpu::TextureDesc& desc) {
        auto texture = device.create(desc);
        if (!texture) FTK_GFX_ERROR("HDRI '{}': {}", path, texture.error());
        return texture ? std::move(*texture) : ftk::gpu::Owned<ftk::gpu::Texture>{};
    };
    const ftk::gpu::TextureDesc cube{
        .kind = ftk::gpu::TextureKind::cube,
        .format = ftk::gpu::Format::rgba16_float,
        .width = HDRI_CUBEMAP_SIZE,
        .height = HDRI_CUBEMAP_SIZE,
        .mips = HDRI_MIP_LEVELS,
        .use = ftk::gpu::TextureUse::storage | ftk::gpu::TextureUse::sampled,
    };
    auto equirect = made(ftk::gpu::TextureDesc{
        .format = ftk::gpu::Format::rgba32_float,
        .width = static_cast<uint32_t>(width),
        .height = static_cast<uint32_t>(height),
        .use = ftk::gpu::TextureUse::sampled,
        .name = "hdri_equirect",
    });
    ftk::gpu::TextureDesc source_desc = cube;
    source_desc.name = "hdri_source";
    auto source = made(source_desc);
    // Cleared until the frame that converts it, as surfaces sample it from
    // the moment it is bound.
    ftk::gpu::TextureDesc cubemap_desc = cube;
    cubemap_desc.initial = ftk::gpu::Clear{0.0f, 0.0f, 0.0f, 1.0f};
    cubemap_desc.name = "hdri_cubemap";
    auto cubemap = made(cubemap_desc);
    if (!equirect || !source || !cubemap) return;

    const size_t floats = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    device.upload().to_texture(equirect,
                               ftk::gpu::TextureRegion{.width = static_cast<uint32_t>(width),
                                                  .height = static_cast<uint32_t>(height)},
                               std::as_bytes(std::span(pixels.get(), floats)),
                               {.after = ftk::gpu::Access::sampled_compute});

    hdri_equirect_ = std::move(equirect);
    hdri_source_ = std::move(source);
    hdri_cubemap_ = std::move(cubemap);
    hdri_pending_ = true;
}

void SkyCubemapPass::convert_hdri(ftk::gpu::CommandList& cmd) {
    ftk::gpu::Device& device = *device_;

    // The image as a cube, then that cube's plain mip chain, which the
    // prefilter samples at the mip its roughness asks for. The prefilter
    // cannot read the cubemap it writes: its lower mips hold what it has
    // already filtered, not the image.
    const ftk::gpu::Sampler equirect_sampler = device.sampler({.filter = ftk::gpu::Filter::linear,
                                                          .mip_filter = ftk::gpu::Filter::nearest,
                                                          .address = ftk::gpu::Address::repeat});
    cmd.barrier(hdri_source_, {}, ftk::gpu::Access::storage_write_compute);
    cmd.set_pipeline(equirect_pipeline_);
    cmd.bind({{"equirect_map", ftk::gpu::sampled(hdri_equirect_, equirect_sampler)},
              {"cubemap_out", ftk::gpu::storage(faces(hdri_source_, 0))}});
    cmd.push(HDRI_CUBEMAP_SIZE);
    cmd.dispatch(groups(HDRI_CUBEMAP_SIZE), groups(HDRI_CUBEMAP_SIZE), 6);
    cmd.barrier(hdri_source_, ftk::gpu::Access::storage_write_compute, ftk::gpu::Access::copy_dst);
    cmd.generate_mipmaps(hdri_source_);
    cmd.barrier(hdri_source_, ftk::gpu::Access::copy_dst, ftk::gpu::Access::sampled_compute);

    // Each mip of the cubemap surfaces sample, filtered for the roughness it
    // stands for; mip 0, roughness 0, is the image itself.
    struct PrefilterPush {
        uint32_t face_size;
        float roughness;
        uint32_t sample_count;
    };
    cmd.set_pipeline(prefilter_pipeline_);
    for (uint32_t mip = 0; mip < HDRI_MIP_LEVELS; ++mip) {
        const uint32_t mip_size = std::max(HDRI_CUBEMAP_SIZE >> mip, 1U);
        const float roughness = static_cast<float>(mip) / static_cast<float>(HDRI_MIP_LEVELS - 1);
        cmd.bind({{"env_cubemap", ftk::gpu::sampled(hdri_source_, cubemap_sampler_)},
                  {"filtered_out", ftk::gpu::storage(faces(hdri_cubemap_, mip))}});
        cmd.push(PrefilterPush{mip_size, roughness, std::min(1024U, 64U * std::max(mip, 1U))});
        cmd.dispatch(groups(mip_size), groups(mip_size), 6);
    }

    // What the conversion took goes once this frame is done with it.
    hdri_equirect_.reset();
    hdri_source_.reset();
    hdri_pending_ = false;
    FTK_GFX_INFO("HDRI cubemap created: {}x{}, {} mips, prefiltered", HDRI_CUBEMAP_SIZE,
                   HDRI_CUBEMAP_SIZE, HDRI_MIP_LEVELS);
}

} // namespace fjell
