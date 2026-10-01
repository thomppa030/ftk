#include "renderer/pass/post_pass.hpp"

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
#include "renderer/pass/exposure_pass.hpp"
#include "renderer/pass_registry.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/post_process_resources.hpp"
#include "renderer/resources/fjsl_parser.hpp"

#include <algorithm>
#include <cmath>

namespace fjell {

// ── Images (per-viewport state) ─────────────────────────────────────────────

void PostPass::Images::create(gpu::Device& gpu, uint32_t width, uint32_t height) {
    written = false;
    auto made = gpu.create(gpu::TextureDesc{
        .format = OUTPUT_FORMAT,
        .width = width,
        .height = height,
        .use = gpu::TextureUse::color_target | gpu::TextureUse::sampled,
        .name = "post_output",
    });
    if (!made) {
        FJELL_GFX_ERROR("{}", made.error());
        return;
    }
    texture = std::move(*made);
    extent = {width, height};
}

void PostPass::Images::destroy() {
    texture.reset();
    extent = {0, 0};
}

void PostPass::Images::resize(gpu::Device& gpu, uint32_t width, uint32_t height) {
    if (texture && extent.x == width && extent.y == height) return;
    texture.reset();
    extent = {0, 0};
    if (width > 0 && height > 0) create(gpu, width, height);
}

// ── PostPass (singleton) ────────────────────────────────────────────────────

namespace {

// Mirrors the push block in fjell_post_template.glsl.
struct PostPush {
    uint32_t material_id;
    uint32_t param_base;
    float exposure;
    float luminance;
    float has_bloom;
    float layer_weight;
};

} // namespace

Result<> PostPass::create(const PassResources& resources) {
    device_ = resources.device;
    globals_ = resources.globals;
    bindless_layout_ = resources.bindless;
    linear_sampler_ = device_->sampler({.filter = gpu::Filter::linear,
                                        .mip_filter = gpu::Filter::nearest,
                                        .address = gpu::Address::clamp});
    nearest_sampler_ = device_->sampler({.filter = gpu::Filter::nearest,
                                         .mip_filter = gpu::Filter::nearest,
                                         .address = gpu::Address::clamp});
    FJELL_GFX_DEBUG("Post pass created");
    return {};
}

void PostPass::destroy() {
    destroy_material_pipelines();
}

void PostPass::destroy_material_pipelines() {
    material_pipelines_.clear();
    default_.ready = false;
}

void PostPass::prepare_frame(const FrameContext& ctx, GpuContext& gpu) {
    // Pipelines are built here rather than in record(): a first sighting of a
    // shader or a hot reload means building a pipeline, which does not
    // belong inside command recording.
    pipeline_cache_ = gpu.pipeline_cache();
    bindless_ = &gpu.bindless_textures();
    ensure_default();
    if (ctx.environment != nullptr) {
        for (const PostVolume& volume : ctx.environment->post_volumes) {
            if (volume.pipeline_id != FogSettings::NO_MATERIAL) {
                ensure_material_pipeline(volume.pipeline_id);
            }
        }
    }

    luminance_ = 0.0f;
    if (!ctx.passes) return;

    // Enable/disable histogram pass based on auto-exposure toggle
    auto* exposure_pass = ctx.passes->try_get<ExposurePass>("exposure");
    if (exposure_pass) {
        exposure_pass->set_enabled(auto_exposure_);
    }

    if (!auto_exposure_ || !exposure_pass || !exposure_pass->enabled()) return;

    // Read the average luminance from this slot's buffer: the frame that
    // wrote it is the one whose fence begin_frame just waited on, so it is
    // complete. The previous frame's buffer would be one frame fresher and
    // may still be in flight.
    float avg_lum = exposure_pass->average_luminance(ctx.frame_index);
    luminance_ = avg_lum;

    // EV100 from average luminance (ISO 100 standard)
    // EV100 = log2(avg_luminance / 0.125)  (where 0.125 = 12.5% middle grey)
    float target_ev = -std::log2(std::max(avg_lum, 1e-5f) / 0.125f);

    // Clamp to allowed range
    target_ev = std::clamp(target_ev, min_ev_, max_ev_);

    // Temporal adaptation: lerp toward target
    float speed = adaptation_speed_ * ctx.delta_time;
    exposure_ += (target_ev - exposure_) * std::min(speed, 1.0f);
}

void PostPass::ensure_default() {
    if (pipeline_cache_ == nullptr || bindless_ == nullptr) return;

    const uint32_t pid = pipeline_cache_->register_shader(std::string(DEFAULT_POST_SHADER));
    const uint32_t generation = pipeline_cache_->generation(pid);
    const bool textured = static_cast<bool>(texture_fallbacks_);
    if (default_.ready && default_.generation == generation && default_.textured == textured) return;
    default_.pipeline_id = pid;
    if (pipeline_cache_->has_compile_error(pid) || generation == 0) {
        // Said once: this runs every frame until a hot reload of the file
        // compiles, and the cache has already logged the compiler's output.
        // A recompile that fails keeps the layer built from the last good one.
        if (reported_entries_.insert(pid).second && !default_.ready) {
            FJELL_GFX_ERROR("Post: '{}' failed to compile; the viewport stays black until it does",
                            DEFAULT_POST_SHADER);
        }
        return;
    }
    reported_entries_.erase(pid);
    if (ensure_material_pipeline(pid) == nullptr) return;

    // The row only carries textures, what an unset slot of each sampler reads
    // (the neutral table for a grade): the values come from the pass's own
    // buffer. Taken once and kept, since nothing hands a row back, and
    // refilled when the shader's samplers may have changed.
    const auto& meta = pipeline_cache_->metadata(pid);
    GpuMaterial row{};
    if (textured) {
        for (const auto& u : meta.uniforms) {
            if (u.type == UniformType::Sampler2D && u.slot < TOTAL_TEXTURE_SLOTS) {
                row.texture_indices[u.slot] = texture_fallbacks_(u.hint);
            }
        }
    }
    if (default_.material_id == ~0u) {
        default_.material_id = bindless_->add_material(row);
    } else {
        bindless_->update_material(default_.material_id, row);
    }

    // The layer everything else applies over: the shader at its declared
    // values, colours marked so a blend toward a volume's mixes them in
    // linear light.
    PostLayer layer;
    for (const auto& u : meta.uniforms) {
        if (u.type == UniformType::Sampler2D) continue;
        NamedValue value;
        value.name = u.name;
        value.count = uniform_float_count(u.type);
        value.srgb = u.hint == UniformHint::Color;
        for (uint32_t i = 0; i < value.count && i < u.default_value.size(); ++i) {
            value.values[i] = u.default_value[i];
        }
        layer.values.push_back(std::move(value));
    }
    layer.look = look_for(PostVolume{});
    default_.layer = std::move(layer);
    default_.generation = generation;
    default_.textured = textured;
    default_.ready = true;
}

PostLook PostPass::look_for(const PostVolume& volume) const {
    const bool own = volume.pipeline_id != FogSettings::NO_MATERIAL
                  && volume.material_id != FogSettings::NO_MATERIAL
                  && material_pipelines_.contains(volume.pipeline_id);
    PostLook look;
    look.pipeline_id = own ? volume.pipeline_id : default_.pipeline_id;
    look.material_id = own ? volume.material_id : default_.material_id;
    if (pipeline_cache_ == nullptr || bindless_ == nullptr
        || look.pipeline_id >= pipeline_cache_->shader_count() || look.material_id == ~0u) {
        return look;
    }
    // Only the slots the shader samples, so two rows that differ where
    // nothing reads are still one look and blend their values.
    const auto& meta = pipeline_cache_->metadata(look.pipeline_id);
    const GpuMaterial& row = bindless_->material_at(look.material_id);
    for (uint32_t i = 0; i < meta.custom_texture_count && i < CUSTOM_TEXTURE_SLOTS; ++i) {
        look.textures[i] = row.texture_indices[PBR_TEXTURE_SLOTS + i];
    }
    return look;
}

const PostPass::MaterialPipeline* PostPass::ensure_material_pipeline(uint32_t pipeline_id) {
    if (pipeline_cache_ == nullptr || pipeline_id >= pipeline_cache_->shader_count()) {
        return nullptr;
    }
    const uint32_t generation = pipeline_cache_->generation(pipeline_id);
    auto it = material_pipelines_.find(pipeline_id);
    if (it != material_pipelines_.end() && it->second.generation == generation) {
        return &it->second;
    }

    const auto& meta = pipeline_cache_->metadata(pipeline_id);
    if (meta.shader_type != ShaderType::Post) {
        if (reported_entries_.insert(pipeline_id).second) {
            FJELL_GFX_ERROR("Post: '{}' is shader_type '{}', a post process volume needs shader_type post",
                            pipeline_cache_->shader_path(pipeline_id),
                            shader_type_name(meta.shader_type));
        }
        return nullptr;
    }
    if (pipeline_cache_->has_compile_error(pipeline_id) || generation == 0) {
        // A failed recompile keeps the pipeline built from the last good one.
        return it != material_pipelines_.end() ? &it->second : nullptr;
    }

    auto fragment = gpu::read_spirv(pipeline_cache_->frag_spv_path(pipeline_id));
    if (!fragment) {
        FJELL_GFX_ERROR("Post: '{}': {}", pipeline_cache_->shader_path(pipeline_id), fragment.error());
        return it != material_pipelines_.end() ? &it->second : nullptr;
    }
    // Set 0 the global UBOs, set 1 the bindless material set, as the sky
    // and fog templates bind them; set 2 the pass inputs.
    gpu::GraphicsPipelineDesc desc{
        .vertex = "shaders/depth_viz.vert",
        .fragment = std::span<const uint32_t>(*fragment),
        .color = {{OUTPUT_FORMAT, gpu::Blend::none}},
        .shared = {globals_, bindless_layout_},
        .name = pipeline_cache_->shader_path(pipeline_id),
    };
    auto base = device_->create(desc);
    // A layer over the base blends by the alpha it writes. The alpha itself
    // blends as "over", which keeps the opaque image opaque: ImGui draws the
    // viewport with alpha blending, so anything less would let the dock show
    // through.
    desc.color = {{OUTPUT_FORMAT, gpu::Blend::alpha}};
    auto over = device_->create(desc);
    if (!base || !over) {
        FJELL_GFX_ERROR("Post: '{}': {}", pipeline_cache_->shader_path(pipeline_id),
                        !base ? base.error() : over.error());
        return it != material_pipelines_.end() ? &it->second : nullptr;
    }

    // The old pipelines may still be referenced by a frame in flight; the
    // device releases them once it is done.
    auto& slot = material_pipelines_[pipeline_id];
    slot.base = std::move(*base);
    slot.over = std::move(*over);
    slot.generation = generation;
    FJELL_GFX_DEBUG("Post: built the pipeline for '{}'", pipeline_cache_->shader_path(pipeline_id));
    return &slot;
}

std::vector<float> PostPass::layer_values(const PostLayer& layer) const {
    if (pipeline_cache_ == nullptr || layer.look.pipeline_id >= pipeline_cache_->shader_count()) {
        return {};
    }
    return resolve_uniform_block(pipeline_cache_->metadata(layer.look.pipeline_id), layer.values);
}

void PostPass::record(const FrameContext& ctx) {
    FJELL_PROFILE_SCOPE_N("record_post");

    auto* output = ctx.pp_resources != nullptr ? ctx.pp_resources->post() : nullptr;
    if (output == nullptr || ctx.target == nullptr) return;

    // Use TAA-resolved color when available, otherwise raw scene color.
    gpu::Texture color_source = ctx.target->color_texture();
    if (auto* taa_imgs = ctx.pp_resources->taa()) {
        uint32_t output_idx = 1 - taa_imgs->write_index;
        if (taa_imgs->history[output_idx]) {
            color_source = taa_imgs->history[output_idx];
        }
    }

    // The day profile's bias rides on top of the project exposure, adapted
    // or fixed alike, so a dusk that should read dark does.
    const float exposure = exposure_ + (ctx.environment != nullptr ? ctx.environment->exposure_bias : 0.0f);

    // The volumes as this viewport's camera stands among them. Until the
    // engine default has compiled there is nothing to draw with, and the
    // viewport is the clear colour rather than a stale image.
    std::vector<PostLayer> layers;
    if (default_.ready) {
        std::vector<PostCandidate> candidates;
        if (ctx.environment != nullptr && !ungraded_) {
            candidates.reserve(ctx.environment->post_volumes.size());
            for (const PostVolume& volume : ctx.environment->post_volumes) {
                candidates.push_back({&volume, look_for(volume)});
            }
        }
        layers = plan_post_layers(default_.layer, candidates, ctx.camera_position);
    }
    // bloom_mip0 is a transient of this run, registered only when declare()
    // asked for it, which it does only when this viewport's graph builds the
    // bloom.
    const Inputs inputs{
        .color = color_source,
        .bloom = ctx.texture("bloom_mip0"),
        .depth = ctx.texture("surface_depth"),
        .normal = ctx.texture("normal"),
        .velocity = ctx.texture("velocity"),
    };
    record_into(ctx, *output, inputs, exposure, layers);
}

void PostPass::record_into(const FrameContext& ctx, Images& imgs, const Inputs& inputs,
                              float exposure, std::span<const PostLayer> layers) {
    if (!imgs.texture || !inputs.color.valid() || !inputs.depth.valid() || !inputs.normal.valid()
        || !inputs.velocity.valid()) {
        return;
    }

    // Every layer's values in one block of this frame's memory, each layer
    // pushing where its own start.
    std::vector<float> values;
    std::vector<uint32_t> bases(layers.size());
    for (size_t i = 0; i < layers.size(); ++i) {
        bases[i] = static_cast<uint32_t>(values.size());
        const std::vector<float> block = layer_values(layers[i]);
        values.insert(values.end(), block.begin(), block.end());
    }
    if (values.empty()) values.push_back(0.0f);

    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Post");
    const gpu::BufferRange params = cmd.transient(values);

    // The base layer covers every pixel, so what was there does not matter.
    // Without one the image is cleared to opaque black, and anything laid
    // over lands on that.
    const bool has_base = !layers.empty() && material_pipelines_.contains(layers.front().look.pipeline_id);
    auto pass = cmd.render({.color = {{.view = imgs.texture,
                                       .load = has_base ? gpu::Load::discard : gpu::Load::clear,
                                       .clear = gpu::Clear{0.0f, 0.0f, 0.0f, 1.0f}}}});

    // With no bloom the scene colour stands in, so the set is complete; the
    // shader reads zero bloom either way.
    const gpu::Texture bloom = inputs.bloom.valid() ? inputs.bloom : inputs.color;
    for (size_t i = 0; i < layers.size(); ++i) {
        const auto built = material_pipelines_.find(layers[i].look.pipeline_id);
        if (built == material_pipelines_.end()) continue;
        pass.set_pipeline(i == 0 ? built->second.base : built->second.over);
        pass.bind(ctx.globals);
        pass.bind(ctx.bindless);
        pass.bind({{"scene_color", gpu::sampled(inputs.color, linear_sampler_)},
                   {"bloom_color", gpu::sampled(bloom, linear_sampler_)},
                   {"surface_depth", gpu::sampled(inputs.depth, nearest_sampler_)},
                   {"PostParams", gpu::storage(params)},
                   {"scene_normal", gpu::sampled(inputs.normal, nearest_sampler_)},
                   {"scene_velocity", gpu::sampled(inputs.velocity, nearest_sampler_)}});
        pass.push(PostPush{
            .material_id = layers[i].look.material_id,
            .param_base = bases[i],
            .exposure = exposure,
            .luminance = luminance_,
            .has_bloom = inputs.bloom.valid() ? 1.0f : 0.0f,
            .layer_weight = layers[i].weight,
        });
        pass.draw(3);
    }
    imgs.written = true;
}

PassSetup PostPass::setup() const {
    return {
        .name = "post",
        .phase = RenderPhase::PostProcess,
        .sub_order = 300,
    };
}

void PostPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    auto color = builder.import_named(ctx, "color");
    builder.read(color, gpu::Access::sampled_fragment);

    // DEPTH in a post shader: the opaque scene with the water laid over it.
    auto depth = builder.import_named(ctx, "surface_depth");
    builder.read(depth, gpu::Access::sampled_fragment);

    // normal_at() and velocity_at(): the resolved G-buffer.
    builder.read(builder.import_named(ctx, "normal"), gpu::Access::sampled_fragment);
    builder.read(builder.import_named(ctx, "velocity"), gpu::Access::sampled_fragment);

    // What the viewport shows, handed to ImGui after the graph.
    if (auto output = builder.import_named_optional(ctx, "post_output"); output.valid()) {
        builder.write(output, gpu::Access::color_attachment);
    }
    // With TAA on, what the post pass reads is the history slot TAA just wrote.
    if (auto taa = builder.import_named_optional(ctx, "taa_history_write"); taa.valid()) {
        builder.read(taa, gpu::Access::sampled_fragment);
    }

    // Declared read on bloom_mip0 so the DAG sees the post ← bloom_us_0
    // edge. Resolving the view happens at record time through the
    // registry; declaring the access lets the graph emit the
    // COLOR_ATTACHMENT → SHADER_READ barrier for us. Name + desc must
    // match what BloomUsPass(0) declares — the graph dedupes virtuals by
    // name, so this create() resolves to the same TrackedImage bloom_us_0
    // writes into.
    if (ctx.bloom_enabled) {
        TextureDesc bloom_desc{};
        bloom_desc.format = gpu::Format::rgba16_float;
        bloom_desc.size_class = SizeClass::viewport;
        bloom_desc.viewport_divisor = 2;
        auto bloom_mip0 = builder.create("bloom_mip0", bloom_desc);
        builder.read(bloom_mip0, gpu::Access::sampled_fragment);
    }

    // The post pass is the frame terminator: its output (the post pp
    // image) goes to the present/ImGui step outside the graph, so
    // mark never_cull so it anchors the backward walk.
    builder.never_cull();
}

} // namespace fjell
