#include "renderer/pipeline_cache.hpp"
#include "ftk/gpu/device.hpp"
#include "ftk/gpu/shader.hpp"
#include "renderer/uniform_types.hpp"
#include "renderer/resources/vertex.hpp"
#include "core/base_path.hpp"
#include "ftk/base/log.hpp"
#include "ftk/base/profiler.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <stdexcept>

namespace fjell {

PipelineCache::PipelineCache(ftk::gpu::Device& gpu, const Targets& targets, const SharedLayouts& layouts)
    : gpu_{gpu}
    , targets_{targets}
    , layouts_{layouts}
    , compiler_{{.shader_dir = base_path::resolve("shaders"),
                 .generated_dir = base_path::resolve("shaders/generated"),
                 .cache_dir = base_path::resolve("shaders/generated/.fjcache"),
                 .generator_version = FJELL_SHADER_CODEGEN_VERSION}}
    , loader_{[this](const std::string& reference) { return base_path::resolve_shared(reference, project_); }} {
    load_templates();

    // Register standard.fjsl as pipeline ID 0
    register_shader("shaders/standard.fjsl");

    FTK_GFX_DEBUG("PipelineCache created with standard shader (ID 0)");
}

void PipelineCache::load_templates() {
    auto load_template = [&](const std::string& name) { return compiler_.read_include(name); };

    // Load the spatial template (required)
    template_source_ = load_template("fjell_spatial_template.glsl");
    sky_template_ = load_template("fjell_sky_template.glsl");
    ui_template_ = load_template("fjell_ui_template.glsl");
    terrain_template_ = load_template("fjell_terrain_template.glsl");
    water_template_ = load_template("fjell_water_template.glsl");
    fog_template_ = load_template("fjell_fog_template.glsl");
    post_template_ = load_template("fjell_post_template.glsl");
    if (template_source_.empty()) {
        throw std::runtime_error("Failed to open spatial template");
    }

    // Load vertex displacement templates (optional — empty if missing)
    mesh_template_ = load_template("fjell_mesh_template.glsl");
    depth_mesh_template_ = load_template("fjell_depth_mesh_template.glsl");
    vertex_template_ = load_template("fjell_vertex_template.glsl");
    depth_vertex_template_ = load_template("fjell_depth_vertex_template.glsl");
    shadow_vertex_template_ = load_template("fjell_shadow_vertex_template.glsl");
    outline_vertex_template_ = load_template("fjell_outline_vertex_template.glsl");
}

PipelineCache::~PipelineCache() = default;

uint32_t PipelineCache::register_shader(const std::string& fjsl_path) {
    auto it = path_to_id_.find(fjsl_path);
    if (it != path_to_id_.end()) {
        return it->second;
    }

    uint32_t id = static_cast<uint32_t>(entries_.size());
    // Built to the side and moved in, rather than compiled in place through a
    // reference into entries_: anything compiling reached back here for would
    // grow the vector and leave that reference dangling.
    Entry entry;
    entry.program.path = fjsl_path;
    compile_entry(entry);
    entries_.push_back(std::move(entry));

    path_to_id_[fjsl_path] = id;
    FTK_GFX_INFO("Registered FJSL shader '{}' as pipeline ID {}", fjsl_path, id);
    return id;
}

void PipelineCache::compile_entry(Entry& entry) {
    FTK_PROFILE_SCOPE_N("fjsl_compile_entry");
    // Build into a temporary entry — only commit on full success
    Entry next{};

    try {
        // Read and parse the .fjsl. A material names its shader the
        // same way whether the engine or the game owns it, so the loader's
        // locator looks in the project first and falls back to the engine.
        next.program = loader_.load(entry.program.path);

        // Pick the template by shader_type. Spatial uses the full PBR
        // template; sky the fullscreen sky template; ui the UI quad template;
        // terrain the layer-blending template drawn by the terrain mesh
        // shader; water the wave template drawn by the water mesh shader; fog
        // the froxel compute template the volumetric fog pass dispatches;
        // post the fullscreen template the post pass draws per layer.
        const ShaderType shader_type = next.program.metadata.shader_type;
        const bool is_ui = shader_type == ShaderType::Ui;
        const bool is_fog = shader_type == ShaderType::Fog;
        // Spatial is the only type drawn by the shared mesh/vertex pipelines;
        // the rest are drawn by a pass that builds its own from the fragment.
        const bool owns_own_pipeline = shader_type != ShaderType::Spatial;
        const std::string& tmpl = [&]() -> const std::string& {
            switch (shader_type) {
                case ShaderType::Sky:     return sky_template_;
                case ShaderType::Ui:      return ui_template_;
                case ShaderType::Terrain: return terrain_template_;
                case ShaderType::Water:   return water_template_;
                case ShaderType::Fog:     return fog_template_;
                case ShaderType::Post:    return post_template_;
                case ShaderType::Spatial: break;
            }
            return template_source_;
        }();
        if (tmpl.empty()) {
            throw std::runtime_error("FJSL: no template loaded for shader_type '"
                                     + std::string(shader_type_name(shader_type)) + "'");
        }

        // Generate the complete fragment shader.
        const std::string frag_source = generate_fragment_shader(next.program.metadata, tmpl);

        // Named after the .fjsl. A fog shader generates a compute shader, and
        // the extension says so to anyone compiling the generated file by hand.
        std::string stem = std::filesystem::path(entry.program.path).stem().string();
        next.generated_frag_path = compiler_.write_generated(stem + (is_fog ? ".comp" : ".frag"), frag_source);

        // Generate vertex shaders to disk (fast, must happen before parallel compile)
        next.has_custom_vertex = !next.program.metadata.vertex_body.empty();

        std::vector<ftk::ShaderCompiler::Job> jobs;

        // The main stage is always compiled: a fragment for every drawn type,
        // the fog pass's medium compute shader for a fog shader, which is
        // compiled once more as the light variant. A ui shader is one
        // generated source compiled in two shapes, screen-space and
        // worldspace; a sky shader once more as the compute shader that fills
        // the lighting cubemap.
        const bool is_sky = shader_type == ShaderType::Sky;
        if (is_fog) {
            jobs.push_back({next.generated_frag_path, stem + ".medium.comp", {}, "compute"});
            jobs.push_back({next.generated_frag_path, stem + ".light.comp", {"FJELL_FOG_LIGHT"}, "compute"});
        } else {
            jobs.push_back({next.generated_frag_path, stem + ".frag", {}, {}});
        }
        if (is_ui) {
            jobs.push_back({next.generated_frag_path, stem + ".world.frag", {"FJELL_UI_WORLD"}, {}});
        }
        if (is_sky) {
            jobs.push_back({next.generated_frag_path, stem + ".cubemap.comp", {"FJELL_SKY_CUBEMAP"}, "compute"});
        }

        if (next.has_custom_vertex) {
            auto add_job = [&](const std::string& vertex_tmpl, const std::string& suffix) {
                if (vertex_tmpl.empty()) return;
                const std::string path = compiler_.write_generated(
                    stem + suffix, generate_vertex_shader(next.program.metadata, vertex_tmpl));
                jobs.push_back({path, stem + suffix, {}, {}});
            };

            add_job(vertex_template_, ".vert");
            add_job(mesh_template_, ".mesh");
            add_job(depth_vertex_template_, ".depth.vert");
            add_job(depth_mesh_template_, ".depth.mesh");
            add_job(shadow_vertex_template_, ".shadow.vert");
            add_job(outline_vertex_template_, ".outline.vert");
        }

        // Compile all shaders in parallel via glslc
        const std::vector<std::string> spvs = compiler_.compile_all(jobs);

        // Collect results
        std::string frag_spv = spvs[0]; // the main stage is always index 0
        size_t idx = 1;
        if (is_fog) {
            next.fog_medium_spv = frag_spv;
            next.fog_light_spv = spvs[idx++];
            frag_spv.clear();
        }
        if (is_ui) {
            next.ui_frag_spv = frag_spv;
            next.ui_world_frag_spv = spvs[idx++];
        }
        if (is_sky) {
            next.sky_cubemap_spv = spvs[idx++];
        }

        if (next.has_custom_vertex) {
            auto collect = [&]() -> std::string {
                return (idx < spvs.size()) ? spvs[idx++] : std::string{};
            };
            if (!vertex_template_.empty()) next.vert_spv = collect();
            if (!mesh_template_.empty()) next.mesh_spv = collect();
            if (!depth_vertex_template_.empty()) next.depth_vert_spv = collect();
            if (!depth_mesh_template_.empty()) next.depth_mesh_spv = collect();
            if (!shadow_vertex_template_.empty()) next.shadow_vert_spv = collect();
            if (!outline_vertex_template_.empty()) next.outline_vert_spv = collect();
        }

        next.frag_spv = frag_spv;

        // Sky, ui and terrain shaders don't participate in any of the spatial
        // pipelines (opaque/transparent/depth-prepass/shadow/outline). SkyPass,
        // UiRenderer and TerrainPass own their pipelines, built from this
        // entry's SPV.
        if (!owns_own_pipeline) {
            create_pipelines(next, frag_spv);
        }

    } catch (const std::exception& e) {
        // Compilation failed — keep old pipelines, store error
        entry.compile_error = e.what();
        FTK_GFX_ERROR("Shader compile failed for '{}': {}", entry.program.path, entry.compile_error);

        return;
    }

    // Success — swap in the new pipelines; the device releases the old ones
    // once the frames drawing with them are done.
    entry.program = std::move(next.program);
    entry.generated_frag_path = std::move(next.generated_frag_path);
    entry.has_custom_vertex = next.has_custom_vertex;
    entry.transparent = std::move(next.transparent);
    entry.gpu_driven = std::move(next.gpu_driven);
    entry.skinned_opaque = std::move(next.skinned_opaque);
    entry.skinned_transparent = std::move(next.skinned_transparent);
    entry.outline = std::move(next.outline);
    entry.frag_spv = std::move(next.frag_spv);
    entry.ui_frag_spv = std::move(next.ui_frag_spv);
    entry.ui_world_frag_spv = std::move(next.ui_world_frag_spv);
    entry.sky_cubemap_spv = std::move(next.sky_cubemap_spv);
    entry.fog_medium_spv = std::move(next.fog_medium_spv);
    entry.fog_light_spv = std::move(next.fog_light_spv);
    entry.vert_spv = std::move(next.vert_spv);
    entry.mesh_spv = std::move(next.mesh_spv);
    entry.depth_vert_spv = std::move(next.depth_vert_spv);
    entry.depth_mesh_spv = std::move(next.depth_mesh_spv);
    entry.shadow_vert_spv = std::move(next.shadow_vert_spv);
    entry.outline_vert_spv = std::move(next.outline_vert_spv);
    entry.compile_error.clear();
    ++entry.generation;
}

namespace {

// A built pipeline, or the reason the entry's compile fails.
ftk::gpu::Owned<ftk::gpu::GraphicsPipeline> built(ftk::Result<ftk::gpu::Owned<ftk::gpu::GraphicsPipeline>> made,
                                        const std::string& what) {
    if (!made) throw std::runtime_error("Failed to create " + what + ": " + made.error());
    return std::move(*made);
}

// Loaded SPIR-V, or the reason the entry's compile fails.
std::vector<uint32_t> spirv_at(const std::string& path) {
    auto words = ftk::gpu::read_spirv(path);
    if (!words) throw std::runtime_error(words.error());
    return std::move(*words);
}

} // namespace

void PipelineCache::create_pipelines(Entry& entry, const std::string& spv_path) {
    const auto& rs = entry.program.metadata.render_state;
    const std::string& path = entry.program.path;
    const std::vector<uint32_t> fragment = spirv_at(spv_path);

    // The material's own vertex stage when it has one; mesh.vert otherwise.
    std::vector<uint32_t> own_vertex;
    ftk::gpu::ShaderCode vertex = "shaders/mesh.vert";
    if (entry.has_custom_vertex && !entry.vert_spv.empty()) {
        own_vertex = spirv_at(entry.vert_spv);
        vertex = std::span<const uint32_t>(own_vertex);
    }

    ftk::gpu::Raster raster{};
    if (rs.cull_disabled) {
        raster.cull = ftk::gpu::Cull::none;
    } else if (rs.cull_front) {
        raster.cull = ftk::gpu::Cull::front;
    } else {
        raster.cull = ftk::gpu::Cull::back;
    }
    // The meshlet culls drop back-facing meshlets unless a draw's data says
    // it keeps them, as it does for exactly the materials that cull no back
    // faces.
    // The colour pass tests against the prepass's depth and writes none.
    const ftk::gpu::Depth depth{.test = !rs.depth_test_disabled, .write = false,
                           .compare = ftk::gpu::Compare::less_equal};

    // The four targets the scene draws to: colour, then metallic/roughness,
    // normal and motion, of which an opaque surface writes two channels and
    // a blended one nothing.
    constexpr ftk::gpu::ChannelMask TWO_CHANNELS = ftk::gpu::Channel::r | ftk::gpu::Channel::g;
    const auto targets = [&](ftk::gpu::Blend color_blend, ftk::gpu::ChannelMask others) {
        return std::vector<ftk::gpu::ColorTarget>{
            {.format = targets_.color, .blend = color_blend},
            {.format = targets_.mr, .write = others},
            {.format = targets_.normal, .write = others},
            {.format = targets_.velocity, .write = others},
        };
    };

    // A blended surface over the colour: by alpha, added by alpha, or
    // multiplied; the target's alpha keeps what is drawn or accumulates.
    ftk::gpu::Blend blend{};
    switch (rs.blend) {
        case BlendMode::Add:
            blend = {true, ftk::gpu::BlendFactor::src_alpha, ftk::gpu::BlendFactor::one, ftk::gpu::BlendFactor::one,
                     ftk::gpu::BlendFactor::one};
            break;
        case BlendMode::Multiply:
            blend = ftk::gpu::Blend::multiply;
            break;
        default: // Alpha
            blend = {true, ftk::gpu::BlendFactor::src_alpha, ftk::gpu::BlendFactor::one_minus_src_alpha,
                     ftk::gpu::BlendFactor::one, ftk::gpu::BlendFactor::zero};
            break;
    }
    const std::vector<ftk::gpu::SharedLayout> vertex_layouts{layouts_.globals, layouts_.bindless,
                                                        layouts_.draw_data};

    // ── Transparent pipeline ──
    entry.transparent = built(gpu_.create(ftk::gpu::GraphicsPipelineDesc{
                                  .vertex = vertex,
                                  .fragment = std::span<const uint32_t>(fragment),
                                  .vertex_layout = Vertex::layout(),
                                  .raster = raster,
                                  .depth = depth,
                                  .color = targets(blend, ftk::gpu::ChannelMask{}),
                                  .depth_format = targets_.depth,
                                  .samples = targets_.samples,
                                  .shared = vertex_layouts,
                                  .name = path,
                              }),
                              "transparent pipeline for: " + path);

    // ── Skinned pipelines ──
    // Disable backface culling for skinned meshes — bone transforms at
    // joints can flip triangle winding, causing holes in the mesh.
    ftk::gpu::Raster skinned_raster = raster;
    skinned_raster.cull = ftk::gpu::Cull::none;
    entry.skinned_opaque = built(gpu_.create(ftk::gpu::GraphicsPipelineDesc{
                                     .vertex = "shaders/skinned_mesh.vert",
                                     .fragment = std::span<const uint32_t>(fragment),
                                     .vertex_layout = SkinnedVertex::layout(),
                                     .raster = skinned_raster,
                                     .depth = depth,
                                     .color = targets(ftk::gpu::Blend::none, TWO_CHANNELS),
                                     .depth_format = targets_.depth,
                                     .samples = targets_.samples,
                                     .shared = vertex_layouts,
                                     .name = path,
                                 }),
                                 "skinned opaque pipeline for: " + path);
    entry.skinned_transparent = built(gpu_.create(ftk::gpu::GraphicsPipelineDesc{
                                          .vertex = "shaders/skinned_mesh.vert",
                                          .fragment = std::span<const uint32_t>(fragment),
                                          .vertex_layout = SkinnedVertex::layout(),
                                          .raster = skinned_raster,
                                          .depth = depth,
                                          .color = targets(blend, ftk::gpu::ChannelMask{}),
                                          .depth_format = targets_.depth,
                                          .samples = targets_.samples,
                                          .shared = vertex_layouts,
                                          .name = path,
                                      }),
                                      "skinned transparent pipeline for: " + path);

    // ── GPU-driven mesh shader pipeline (if mesh shaders available) ──
    if (layouts_.meshlets.valid()) {
        std::vector<uint32_t> own_mesh;
        ftk::gpu::ShaderCode mesh = "shaders/mesh_gpu_driven.mesh";
        if (entry.has_custom_vertex && !entry.mesh_spv.empty()) {
            own_mesh = spirv_at(entry.mesh_spv);
            mesh = std::span<const uint32_t>(own_mesh);
        }
        entry.gpu_driven = built(gpu_.create(ftk::gpu::GraphicsPipelineDesc{
                                     .task = "shaders/mesh_gpu_driven.task",
                                     .mesh = mesh,
                                     .fragment = std::span<const uint32_t>(fragment),
                                     .raster = raster,
                                     .depth = depth,
                                     .color = targets(ftk::gpu::Blend::none, TWO_CHANNELS),
                                     .depth_format = targets_.depth,
                                     .samples = targets_.samples,
                                     .shared = {layouts_.globals, layouts_.bindless, layouts_.draw_data,
                                                layouts_.meshlets},
                                     .name = path,
                                 }),
                                 "GPU-driven pipeline for: " + path);
    }

    // ── Outline pipeline ──
    // Back faces pushed out along their normals, blended over the colour,
    // no depth written.
    std::vector<uint32_t> own_outline;
    ftk::gpu::ShaderCode outline_vertex = "shaders/outline.vert";
    if (entry.has_custom_vertex && !entry.outline_vert_spv.empty()) {
        own_outline = spirv_at(entry.outline_vert_spv);
        outline_vertex = std::span<const uint32_t>(own_outline);
    }
    ftk::gpu::Raster outline_raster = raster;
    outline_raster.cull = ftk::gpu::Cull::front;
    const ftk::gpu::Blend over{true, ftk::gpu::BlendFactor::src_alpha, ftk::gpu::BlendFactor::one_minus_src_alpha,
                          ftk::gpu::BlendFactor::one, ftk::gpu::BlendFactor::zero};
    entry.outline = built(gpu_.create(ftk::gpu::GraphicsPipelineDesc{
                              .vertex = outline_vertex,
                              .fragment = "shaders/outline.frag",
                              .vertex_layout = Vertex::layout(),
                              .raster = outline_raster,
                              .depth = depth,
                              .color = targets(over, ftk::gpu::ChannelMask{}),
                              .depth_format = targets_.depth,
                              .samples = targets_.samples,
                              .shared = vertex_layouts,
                              .name = path,
                          }),
                          "outline pipeline for: " + path);
}

void PipelineCache::reload(uint32_t id) {
    if (id >= entries_.size()) return;
    compile_entry(entries_[id]);
    FTK_GFX_INFO("Reloaded FJSL pipeline {}", id);
}

uint32_t PipelineCache::reload_changed(const std::vector<std::string>& changed_paths) {
    if (changed_paths.empty()) return 0;

    std::vector<uint32_t> to_reload;
    for (uint32_t id = 0; id < entries_.size(); ++id) {
        const FjslProgram& program = entries_[id].program;
        if (std::ranges::any_of(changed_paths, [&](const std::string& changed) {
                return program.depends_on(changed);
            })) {
            to_reload.push_back(id);
        }
    }
    if (to_reload.empty()) return 0;

    for (uint32_t id : to_reload) {
        compile_entry(entries_[id]);
        FTK_GFX_INFO("Reloaded FJSL pipeline {} ('{}')", id, entries_[id].program.path);
    }
    return static_cast<uint32_t>(to_reload.size());
}

void PipelineCache::recreate_all() {
    // Everything is rebuilt because an include may have changed, and the
    // templates every shader is generated against are includes.
    compiler_.rehash_includes();
    load_templates();
    int failures = 0;
    for (auto& entry : entries_) {
        compile_entry(entry);
        if (!entry.compile_error.empty()) failures++;
    }
    if (failures > 0) {
        FTK_GFX_WARN("Recreated FJSL pipelines: {}/{} failed", failures, entries_.size());
    } else {
        FTK_GFX_INFO("Recreated all {} FJSL pipelines", entries_.size());
    }
}

ftk::gpu::GraphicsPipeline PipelineCache::transparent_pipeline(uint32_t id) const {
    const ftk::gpu::GraphicsPipeline p = entries_[id].transparent;
    return p.valid() ? p : ftk::gpu::GraphicsPipeline(entries_[0].transparent);
}

ftk::gpu::GraphicsPipeline PipelineCache::gpu_driven_pipeline(uint32_t id) const {
    const ftk::gpu::GraphicsPipeline p = entries_[id].gpu_driven;
    return p.valid() ? p : ftk::gpu::GraphicsPipeline(entries_[0].gpu_driven);
}

ftk::gpu::GraphicsPipeline PipelineCache::skinned_opaque_pipeline(uint32_t id) const {
    const ftk::gpu::GraphicsPipeline p = entries_[id].skinned_opaque;
    return p.valid() ? p : ftk::gpu::GraphicsPipeline(entries_[0].skinned_opaque);
}

ftk::gpu::GraphicsPipeline PipelineCache::skinned_transparent_pipeline(uint32_t id) const {
    const ftk::gpu::GraphicsPipeline p = entries_[id].skinned_transparent;
    return p.valid() ? p : ftk::gpu::GraphicsPipeline(entries_[0].skinned_transparent);
}

ftk::gpu::GraphicsPipeline PipelineCache::outline_pipeline(uint32_t id) const {
    const ftk::gpu::GraphicsPipeline p = entries_[id].outline;
    return p.valid() ? p : ftk::gpu::GraphicsPipeline(entries_[0].outline);
}

ftk::gpu::Cull PipelineCache::cull(uint32_t id) const {
    const auto& rs = entries_[id].program.metadata.render_state;
    if (rs.cull_disabled) return ftk::gpu::Cull::none;
    if (rs.cull_front) return ftk::gpu::Cull::front;
    return ftk::gpu::Cull::back;
}

const ShaderMetadata& PipelineCache::metadata(uint32_t id) const {
    return entries_[id].program.metadata;
}

bool PipelineCache::has_custom_vertex(uint32_t id) const {
    return entries_[id].has_custom_vertex;
}

const std::string& PipelineCache::depth_vert_spv(uint32_t id) const {
    return entries_[id].depth_vert_spv;
}

const std::string& PipelineCache::depth_mesh_spv(uint32_t id) const {
    return entries_[id].depth_mesh_spv;
}

const std::string& PipelineCache::shadow_vert_spv(uint32_t id) const {
    return entries_[id].shadow_vert_spv;
}

const std::string& PipelineCache::frag_spv_path(uint32_t id) const {
    return entries_[id].frag_spv;
}

const std::string& PipelineCache::ui_frag_spv(uint32_t id) const {
    return entries_[id].ui_frag_spv;
}

const std::string& PipelineCache::ui_world_frag_spv(uint32_t id) const {
    return entries_[id].ui_world_frag_spv;
}

const std::string& PipelineCache::sky_cubemap_spv(uint32_t id) const {
    return entries_[id].sky_cubemap_spv;
}

const std::string& PipelineCache::fog_medium_spv(uint32_t id) const {
    return entries_[id].fog_medium_spv;
}

const std::string& PipelineCache::fog_light_spv(uint32_t id) const {
    return entries_[id].fog_light_spv;
}

} // namespace fjell
