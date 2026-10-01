#include "renderer/pass/sky_pass.hpp"

#include "core/base_path.hpp"
#include "core/project.hpp"
#include "environment/environment_state.hpp"
#include "ftk/base/log.hpp"
#include "ftk/base/profiler.hpp"
#include "ftk/framegraph/pass_builder.hpp"
#include "ftk/gpu/device.hpp"
#include "ftk/gpu/render_encoder.hpp"
#include "ftk/gpu/shader.hpp"
#include "renderer/bindless.hpp"
#include "renderer/gpu_context.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/viewport_source.hpp"

#include <array>
#include <filesystem>
#include <span>
#include <vector>

namespace fjell {

void SkyPass::prepare_frame(const FrameContext& ctx, GpuContext& /*gpu*/) {
    if (ctx.environment == nullptr) return;

    const auto& sky = ctx.environment->sky;
    uniform_values_ = ctx.environment->sky_uniforms;
    top_color_ = sky.top_color;
    bottom_color_ = sky.bottom_color;
    sky_shader_ = sky.sky_shader;
    if (!sky_shader_.empty()) {
        mode_ = Mode::Fjsl;
    } else if (sky.atmosphere_enabled) {
        mode_ = Mode::Atmosphere;
    } else if (!sky.hdri_path.empty()) {
        mode_ = Mode::Hdri;
    } else {
        mode_ = Mode::Gradient;
    }
    has_hdri_ = !sky.hdri_path.empty();
    atm_rayleigh_scale_ = sky.atmosphere_rayleigh_scale;
    atm_mie_scale_      = sky.atmosphere_mie_scale;
    atm_mie_g_          = sky.atmosphere_mie_g;
    atm_mie_coeff_      = sky.atmosphere_mie_coeff;
    atm_sun_intensity_  = sky.atmosphere_sun_intensity;
    atm_hdri_floor_     = sky.atmosphere_hdri_floor;

    if (mode_ == Mode::Fjsl) {
        ensure_fjsl_pipeline(cached_resources_);
        apply_uniform_values();
    }
}

std::string canonical_sky_shader_path(const std::string& sky_shader, const Project* project) {
    if (sky_shader.find("..") == std::string::npos && !sky_shader.starts_with("/")) {
        return sky_shader;
    }
    std::string resolved = base_path::resolve_asset(sky_shader, project);
    std::error_code ec;
    auto abs = std::filesystem::weakly_canonical(resolved, ec);
    return ec ? resolved : abs.string();
}

void SkyPass::write_default_params(const ShaderMetadata& meta, uint32_t param_offset) {
    std::vector<float> seed(meta.custom_param_count, 0.0f);
    for (const auto& u : meta.uniforms) {
        if (u.type == UniformType::Sampler2D) continue;
        uint32_t count = uniform_float_count(u.type);
        for (uint32_t i = 0; i < count && i < u.default_value.size(); ++i) {
            seed[u.slot + i] = u.default_value[i];
        }
        encode_uniform_for_gpu(u, std::span{seed}.subspan(u.slot, count));
    }
    fjsl_bindless_->write_params(param_offset, seed.data(), meta.custom_param_count);
}

void SkyPass::apply_uniform_values() {
    if (!fjsl_pipeline_ || fjsl_pipeline_cache_ == nullptr || fjsl_bindless_ == nullptr) return;
    // The pipeline and the material row are taken together, so one implies
    // the other — but reading a row that was never taken would index past the
    // end of the material buffer, which is worth a line to rule out.
    if (fjsl_material_id_ == ~0u) return;
    if (uniform_values_applied_ && uniform_values_ == applied_uniform_values_) return;

    const auto& meta = fjsl_pipeline_cache_->metadata(fjsl_pipeline_id_);
    if (meta.custom_param_count == 0) {
        applied_uniform_values_ = uniform_values_;
        uniform_values_applied_ = true;
        return;
    }
    const uint32_t param_offset = fjsl_bindless_->material_at(fjsl_material_id_).param_offset;

    // Start from the defaults so a value that went away falls back to the
    // .fjsl, then lay every named value over its uniform. A value shorter
    // than the uniform keeps the default in the components it leaves out.
    write_default_params(meta, param_offset);
    for (const auto& value : uniform_values_) {
        for (const auto& u : meta.uniforms) {
            if (u.type == UniformType::Sampler2D || u.name != value.name) continue;
            const uint32_t count = uniform_float_count(u.type);
            std::array<float, 4> temp{};
            for (uint32_t i = 0; i < count && i < u.default_value.size(); ++i) temp[i] = u.default_value[i];
            for (uint32_t i = 0; i < count && i < value.count; ++i) temp[i] = value.values[i];
            encode_uniform_for_gpu(u, std::span{temp}.first(count));
            fjsl_bindless_->write_params(param_offset + u.slot, temp.data(), count);
            break;
        }
    }
    applied_uniform_values_ = uniform_values_;
    uniform_values_applied_ = true;
}

namespace {

// The sky's pipeline: a fullscreen triangle behind everything, depth-tested
// so it only fills pixels no geometry reached, writing colour there and
// clearing metallic/roughness and normal, which the lit screen-space passes
// read.
gpu::GraphicsPipelineDesc sky_pipeline(const PassResources& resources, gpu::ShaderCode fragment,
                                       std::vector<gpu::SharedLayout> shared, std::string_view name) {
    constexpr gpu::ChannelMask TWO_CHANNELS = gpu::Channel::r | gpu::Channel::g;
    return gpu::GraphicsPipelineDesc{
        .vertex = "shaders/sky.vert",
        .fragment = fragment,
        .depth = {.test = true, .write = false, .compare = gpu::Compare::less_equal},
        .color = {{.format = resources.scene_color_format},
                  {.format = resources.scene_mr_format, .write = TWO_CHANNELS},
                  {.format = resources.scene_normal_format, .write = TWO_CHANNELS}},
        .depth_format = resources.scene_depth_format,
        .samples = resources.samples,
        .shared = std::move(shared),
        .name = name,
    };
}

} // namespace

Result<> SkyPass::create(const PassResources& resources) {
    cached_resources_ = resources;
    FJELL_TRY(pipeline_, resources.device->create(
                             sky_pipeline(resources, "shaders/sky.frag", {resources.globals}, "sky")));
    FJELL_GFX_DEBUG("Sky pass created");
    return {};
}

void SkyPass::post_create(GpuContext& gpu, PassRegistry& /*passes*/) {
    fjsl_pipeline_cache_ = gpu.pipeline_cache();
    fjsl_bindless_ = &gpu.bindless_textures();
}

void SkyPass::destroy() {
    fjsl_pipeline_.reset();
    fjsl_pipeline_shader_.clear();
    pipeline_.reset();
}

PassSetup SkyPass::setup() const {
    return {
        .name = "sky",
        .phase = RenderPhase::Color,
        .sub_order = 8, // after opaque (0) has laid down depth, before the
                        // grid (10) which draws on top of the sky
    };
}

void SkyPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    auto msaa_color = builder.import_named(ctx, "msaa_color");
    auto msaa_mr = builder.import_named(ctx, "msaa_mr");
    auto msaa_normal = builder.import_named(ctx, "msaa_normal");
    auto msaa_depth = builder.import_named(ctx, "msaa_depth");
    // The sky clears metallic-roughness and normal on background pixels.
    // GTAO, SS-GI and RT reflections all sample those targets, so dropping
    // the writes would leave them reading whatever the opaque clear left.
    builder.write(msaa_color, gpu::Access::color_attachment);
    builder.write(msaa_mr, gpu::Access::color_attachment);
    builder.write(msaa_normal, gpu::Access::color_attachment);
    // Depth-tested so the sky only fills pixels no geometry reached, but
    // never written.
    builder.read(msaa_depth, gpu::Access::depth_attachment_read);
    // The backward cull walk starts at the post pass and follows readers of the
    // resolved `color`. With MSAA on, `msaa_color` is a separate image
    // nothing samples, so without this anchor the sky is culled entirely.
    builder.has_side_effects();
}

void SkyPass::record(const FrameContext& ctx) {
    if (!ctx.target) return;

    FJELL_PROFILE_SCOPE_N("record_sky");
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Sky");

    // Over what the opaque pass drew: colour, metallic/roughness and normal
    // loaded, depth only tested.
    auto* target = ctx.target;
    auto pass = cmd.render({
        .color = {{.view = target->msaa_color_texture()},
                  {.view = target->msaa_mr_texture()},
                  {.view = target->msaa_normal_texture()}},
        .depth = gpu::DepthAttachment{.view = target->msaa_depth_texture(),
                                      .access = gpu::Access::depth_attachment_read},
    });

    // A backdrop replaces whatever sky is configured, custom shader included;
    // the sky cubemap the surfaces reflect is built by SkyCubemapPass and
    // untouched by this.
    const bool has_backdrop = ctx.backdrop != nullptr;
    if (!has_backdrop && mode_ == Mode::Fjsl && fjsl_pipeline_ && fjsl_material_id_ != ~0u) {
        struct {
            float viewport_width;
            float viewport_height;
            uint32_t sky_material_id;
            uint32_t face_size;   // the cubemap variant's; unused by the fragment
        } push{
            static_cast<float>(target->extent().x),
            static_cast<float>(target->extent().y),
            fjsl_material_id_,
            0u,
        };
        pass.set_pipeline(fjsl_pipeline_);
        pass.bind(ctx.globals);
        pass.bind(ctx.bindless);
        pass.push(push);
        pass.draw(3);
        return;
    }

    struct {
        glm::vec4 top;
        glm::vec4 bottom;
        float viewport_width;
        float viewport_height;
        float sky_mode;
        float atmosphere_rayleigh_scale;
        float atmosphere_mie_scale;
        float atmosphere_mie_g;
        float atmosphere_sun_intensity;
        float atmosphere_mie_coeff;
        float atmosphere_hdri_floor;
        float atmosphere_has_hdri;
    } push{
        glm::vec4(has_backdrop ? ctx.backdrop->top : top_color_, 1.0f),
        glm::vec4(has_backdrop ? ctx.backdrop->bottom : bottom_color_, 1.0f),
        static_cast<float>(target->extent().x),
        static_cast<float>(target->extent().y),
        static_cast<float>(has_backdrop ? Mode::Gradient : mode_),
        atm_rayleigh_scale_,
        atm_mie_scale_,
        atm_mie_g_,
        atm_sun_intensity_,
        atm_mie_coeff_,
        atm_hdri_floor_,
        has_hdri_ ? 1.0f : 0.0f
    };
    pass.set_pipeline(pipeline_);
    pass.bind(ctx.globals);
    pass.push(push);
    pass.draw(3);
}

void SkyPass::ensure_fjsl_pipeline(const PassResources& resources) {
    if (!fjsl_pipeline_cache_ || !fjsl_bindless_) {
        return;  // wiring not provided — silently skip; gradient fallback runs.
    }
    if (sky_shader_ == fjsl_pipeline_shader_ && fjsl_pipeline_ &&
        fjsl_pipeline_cache_->generation(fjsl_pipeline_id_) == fjsl_pipeline_generation_) {
        return;  // already built for this path and this compile of it.
    }

    // The sky is rebuilt from inside record(), so the pipeline being
    // replaced may still be referenced by a frame in flight: the device
    // releases it once the GPU is done.
    fjsl_pipeline_.reset();
    fjsl_pipeline_shader_.clear();

    // The inspector stores project-relative paths (e.g. "../Fjell/shaders/foo.fjsl"
    // when the engine shader lives outside the project tree). Resolve to an
    // absolute path before handing to PipelineCache, which otherwise treats
    // strings as engine-relative and would double-prefix the "../" form.
    const std::string canonical = canonical_sky_shader_path(sky_shader_, project_);

    // Compile the shader through PipelineCache (parses metadata, runs
    // glslc, doesn't create any spatial pipelines for shader_type sky).
    uint32_t pid = fjsl_pipeline_cache_->register_shader(canonical);
    if (fjsl_pipeline_cache_->has_compile_error(pid)) {
        FJELL_GFX_ERROR("SkyPass: FJSL '{}' failed to compile: {}",
                        sky_shader_, fjsl_pipeline_cache_->compile_error(pid));
        return;
    }
    const auto& meta = fjsl_pipeline_cache_->metadata(pid);
    if (meta.shader_type != ShaderType::Sky) {
        FJELL_GFX_ERROR("SkyPass: '{}' is shader_type '{}', expected 'sky'",
                        sky_shader_, shader_type_name(meta.shader_type));
        return;
    }
    const std::string& frag_spv = fjsl_pipeline_cache_->frag_spv_path(pid);

    // The sky's material row and its params slice, taken once and kept. A
    // rebuild reuses them: nothing hands a row back, so allocating on every
    // recompile leaked one per shader reload, and the buffer behind them is
    // not endless.
    //
    // A slice is only retaken when the shader wants more room than the last
    // one had, which is the one case the old slice cannot serve.
    GpuMaterial gpu{};
    if (meta.custom_param_count > 0) {
        if (meta.custom_param_count > fjsl_param_count_) {
            fjsl_param_offset_ = fjsl_bindless_->allocate_params(meta.custom_param_count);
            fjsl_param_count_ = meta.custom_param_count;
        }
        gpu.param_offset = fjsl_param_offset_;
        gpu.param_count  = meta.custom_param_count;
        write_default_params(meta, gpu.param_offset);
    }
    // The slice holds defaults again; the environment's values go back on top
    // on the next apply.
    uniform_values_applied_ = false;
    if (fjsl_material_id_ == ~0u) {
        fjsl_material_id_ = fjsl_bindless_->add_material(gpu);
    } else {
        fjsl_bindless_->update_material(fjsl_material_id_, gpu);
    }
    fjsl_pipeline_id_ = pid;
    fjsl_pipeline_generation_ = fjsl_pipeline_cache_->generation(pid);

    // The built-in sky's triangle with the FJSL fragment; set 0 the globals,
    // set 1 the bindless material set.
    auto fragment = gpu::read_spirv(frag_spv);
    if (!fragment) {
        FJELL_GFX_ERROR("SkyPass: '{}': {}", sky_shader_, fragment.error());
        return;
    }
    auto made = resources.device->create(sky_pipeline(resources, std::span<const uint32_t>(*fragment),
                                                      {resources.globals, resources.bindless}, sky_shader_));
    if (!made) {
        FJELL_GFX_ERROR("SkyPass: failed to create FJSL sky pipeline for '{}': {}", sky_shader_, made.error());
        return;
    }
    fjsl_pipeline_ = std::move(*made);

    fjsl_pipeline_shader_ = sky_shader_;
    FJELL_GFX_INFO("SkyPass: built FJSL sky pipeline for '{}' (material_id={})",
                   sky_shader_, fjsl_material_id_);
}

} // namespace fjell
