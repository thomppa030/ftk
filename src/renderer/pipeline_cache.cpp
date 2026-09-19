#include "renderer/pipeline_cache.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/resources/shader_utils.hpp"
#include "renderer/uniform_types.hpp"
#include "renderer/resources/vertex.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "platform/platform.hpp"

#include <glm/glm.hpp>

#include <filesystem>
#include <fstream>
#include <future>
#include <sstream>
#include <stdexcept>

namespace fjell {

namespace {

// FNV-1a. Not std::hash: that is unspecified and free to differ between
// runs and standard libraries, which is fine for a lookup table and fatal
// for a key written to disk and read back by a later build.
constexpr uint64_t FNV_OFFSET = 1469598103934665603ULL;
constexpr uint64_t FNV_PRIME = 1099511628211ULL;

uint64_t hash_bytes(std::string_view bytes, uint64_t seed = FNV_OFFSET) {
    uint64_t h = seed;
    for (unsigned char c : bytes) {
        h ^= c;
        h *= FNV_PRIME;
    }
    return h;
}

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// What a compiled shader is a function of, beyond its own generated source:
// the includes it pulls in, the defines it was built with, the stage it was
// built as, and the compiler itself.
//
// FJELL_SHADER_CODEGEN_VERSION stands in for the generator. The generated
// GLSL is an output of fjsl_parser.cpp, so changing how that emits must
// invalidate everything — the .fjsl did not move but what it produces did.
// Bump it whenever generation changes shape.
constexpr uint32_t FJELL_SHADER_CODEGEN_VERSION = 1;

} // namespace

PipelineCache::PipelineCache(Device& device,
                             VkFormat color_format, VkFormat depth_format,
                             VkDescriptorSetLayout global_layout,
                             VkDescriptorSetLayout material_layout,
                             VkDescriptorSetLayout draw_data_layout,
                             VkSampleCountFlagBits samples,
                             VkDescriptorSetLayout meshlet_layout,
                             VkPipelineCache vk_cache)
    : device_{device}
    , color_format_{color_format}
    , depth_format_{depth_format}
    , global_layout_{global_layout}
    , material_layout_{material_layout}
    , draw_data_layout_{draw_data_layout}
    , meshlet_layout_{meshlet_layout}
    , samples_{samples}
    , vk_cache_{vk_cache} {

    shader_source_dir_ = base_path::resolve("shaders");
    generated_dir_ = base_path::resolve("shaders/generated");
    std::filesystem::create_directories(generated_dir_);
    spv_cache_dir_ = generated_dir_ + "/.fjcache";
    std::filesystem::create_directories(spv_cache_dir_);

    // One hash over every include, in a fixed order so two runs over the same
    // files agree. Taken once: they cannot change while the engine is
    // running without a reload, and a reload rebuilds everything anyway.
    {
        std::vector<std::filesystem::path> includes;
        std::error_code ec;
        for (const auto& entry :
             std::filesystem::directory_iterator(shader_source_dir_ + "/include", ec)) {
            if (entry.is_regular_file(ec)) includes.push_back(entry.path());
        }
        std::sort(includes.begin(), includes.end());
        uint64_t h = FNV_OFFSET;
        for (const auto& path : includes) {
            h = hash_bytes(path.filename().string(), h);
            h = hash_bytes(read_file(path.string()), h);
        }
        include_hash_ = h;
    }

    auto load_template = [&](const std::string& name) -> std::string {
        std::string path = shader_source_dir_ + "/include/" + name;
        std::ifstream f(path);
        if (!f.is_open()) return {};
        std::ostringstream s;
        s << f.rdbuf();
        return s.str();
    };

    // Load the spatial template (required)
    template_source_ = load_template("fjell_spatial_template.glsl");
    sky_template_ = load_template("fjell_sky_template.glsl");
    ui_template_ = load_template("fjell_ui_template.glsl");
    terrain_template_ = load_template("fjell_terrain_template.glsl");
    water_template_ = load_template("fjell_water_template.glsl");
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

    create_layout();

    // Register standard.fjsl as pipeline ID 0
    register_shader("shaders/standard.fjsl");

    FJELL_GFX_DEBUG("PipelineCache created with standard shader (ID 0)");
}

PipelineCache::~PipelineCache() {
    auto vk = device_.handle();
    for (auto& e : entries_) {
        destroy_entry_pipelines(e);
    }
    if (layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(vk, layout_, nullptr);
        layout_ = VK_NULL_HANDLE;
    }
    if (gpu_driven_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(vk, gpu_driven_layout_, nullptr);
        gpu_driven_layout_ = VK_NULL_HANDLE;
    }
    if (outline_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(vk, outline_layout_, nullptr);
        outline_layout_ = VK_NULL_HANDLE;
    }
}

void PipelineCache::create_layout() {
    // Vertex pipeline layout: set 0/1/2
    std::array set_layouts = {global_layout_, material_layout_, draw_data_layout_};

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(PbrPushConstants);

    VkPipelineLayoutCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    ci.setLayoutCount = static_cast<uint32_t>(set_layouts.size());
    ci.pSetLayouts = set_layouts.data();
    ci.pushConstantRangeCount = 1;
    ci.pPushConstantRanges = &push_range;

    if (vkCreatePipelineLayout(device_.handle(), &ci, nullptr, &layout_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create pipeline cache layout");
    }

    // GPU-driven mesh shader layout: set 0/1/2/3 + push constant for bin base offset
    if (meshlet_layout_ != VK_NULL_HANDLE) {
        std::array mesh_set_layouts = {global_layout_, material_layout_,
                                       draw_data_layout_, meshlet_layout_};
        VkPushConstantRange gpu_push{};
        gpu_push.stageFlags = VK_SHADER_STAGE_TASK_BIT_EXT;
        gpu_push.offset = 0;
        gpu_push.size = sizeof(uint32_t); // bin_base_offset

        VkPipelineLayoutCreateInfo gpu_ci{};
        gpu_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        gpu_ci.setLayoutCount = static_cast<uint32_t>(mesh_set_layouts.size());
        gpu_ci.pSetLayouts = mesh_set_layouts.data();
        gpu_ci.pushConstantRangeCount = 1;
        gpu_ci.pPushConstantRanges = &gpu_push;

        if (vkCreatePipelineLayout(device_.handle(), &gpu_ci, nullptr, &gpu_driven_layout_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create GPU-driven pipeline layout");
        }
    }

    // Outline layout: same sets as vertex pipeline, push constant = vec4 color + float thickness
    {
        VkPushConstantRange outline_push{};
        outline_push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        outline_push.size = sizeof(glm::vec4) + sizeof(float);

        VkPipelineLayoutCreateInfo outline_ci{};
        outline_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        outline_ci.setLayoutCount = static_cast<uint32_t>(set_layouts.size());
        outline_ci.pSetLayouts = set_layouts.data();
        outline_ci.pushConstantRangeCount = 1;
        outline_ci.pPushConstantRanges = &outline_push;

        if (vkCreatePipelineLayout(device_.handle(), &outline_ci, nullptr, &outline_layout_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create outline pipeline layout");
        }
    }

}

uint32_t PipelineCache::register_shader(const std::string& fjsl_path) {
    auto it = path_to_id_.find(fjsl_path);
    if (it != path_to_id_.end()) {
        return it->second;
    }

    uint32_t id = static_cast<uint32_t>(entries_.size());
    entries_.emplace_back();
    auto& entry = entries_.back();
    entry.fjsl_path = fjsl_path;

    compile_entry(entry);

    path_to_id_[fjsl_path] = id;
    FJELL_GFX_INFO("Registered FJSL shader '{}' as pipeline ID {}", fjsl_path, id);
    return id;
}

void PipelineCache::compile_entry(Entry& entry) {
    FJELL_PROFILE_SCOPE_N("fjsl_compile_entry");
    // Build into a temporary entry — only commit on full success
    Entry next{};
    next.fjsl_path = entry.fjsl_path;

    try {
        // Read the .fjsl source. A material names its shader the same way
        // whether the engine or the game owns it, so look in the project
        // first and fall back to the engine.
        std::string full_path = base_path::resolve_shared(entry.fjsl_path, project_);
        if (full_path.empty()) {
            throw std::runtime_error("FJSL file not found in project or engine: "
                                     + entry.fjsl_path);
        }
        std::ifstream f(full_path);
        if (!f.is_open()) {
            throw std::runtime_error("Failed to open FJSL file: " + full_path);
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        std::string source = ss.str();

        // Parse FJSL
        next.metadata = parse_fjsl(source);
        // The name a compiler error should carry. The parser works on text and
        // does not know where it came from.
        next.metadata.source_path = entry.fjsl_path;

        // The parser collects problems rather than throwing on the first, so a
        // shader with two mistakes reports both. Warnings are logged and the
        // build continues; an error means the metadata cannot be built from.
        log_fjsl_diagnostics(next.metadata, entry.fjsl_path);
        std::string parse_errors;
        for (const auto& d : next.metadata.diagnostics) {
            if (d.severity != FjslSeverity::error) continue;
            if (!parse_errors.empty()) parse_errors += "\n";
            parse_errors += entry.fjsl_path + ":" + std::to_string(d.line)
                          + ":" + std::to_string(d.column) + ": " + d.message;
        }
        if (!parse_errors.empty()) {
            throw std::runtime_error(parse_errors);
        }

        // Pick the template by shader_type. Spatial uses the full PBR
        // template; sky the fullscreen sky template; ui the UI quad template;
        // terrain the layer-blending template drawn by the terrain mesh
        // shader; water the wave template drawn by the water mesh shader.
        const ShaderType shader_type = next.metadata.shader_type;
        const bool is_ui = shader_type == ShaderType::Ui;
        // Spatial is the only type drawn by the shared mesh/vertex pipelines;
        // the rest are drawn by a pass that builds its own from the fragment.
        const bool owns_own_pipeline = shader_type != ShaderType::Spatial;
        const std::string& tmpl = [&]() -> const std::string& {
            switch (shader_type) {
                case ShaderType::Sky:     return sky_template_;
                case ShaderType::Ui:      return ui_template_;
                case ShaderType::Terrain: return terrain_template_;
                case ShaderType::Water:   return water_template_;
                case ShaderType::Spatial: break;
            }
            return template_source_;
        }();
        if (tmpl.empty()) {
            throw std::runtime_error("FJSL: no template loaded for shader_type '"
                                     + std::string(shader_type_name(shader_type)) + "'");
        }

        // Generate complete fragment shader
        std::string frag_source = generate_fragment_shader(next.metadata, tmpl);

        // Determine output name from fjsl filename
        std::string stem = std::filesystem::path(entry.fjsl_path).stem().string();
        next.generated_frag_path = generated_dir_ + "/" + stem + ".frag";

        // Write generated .frag
        {
            std::ofstream out(next.generated_frag_path);
            if (!out.is_open()) {
                throw std::runtime_error("Failed to write generated shader: " + next.generated_frag_path);
            }
            out << frag_source;
        }

        // Generate vertex shaders to disk (fast, must happen before parallel compile)
        next.has_custom_vertex = !next.metadata.vertex_body.empty();

        struct ShaderJob {
            std::string source_path;
            std::string output_name;
            std::vector<std::string> defines;
            std::string stage;  // glslc -fshader-stage; empty infers from the extension
        };
        std::vector<ShaderJob> jobs;

        // Fragment is always compiled. A ui shader is one generated source
        // compiled in two shapes, screen-space and worldspace; a sky shader
        // once more as the compute shader that fills the lighting cubemap.
        const bool is_sky = shader_type == ShaderType::Sky;
        jobs.push_back({next.generated_frag_path, stem + ".frag", {}, {}});
        if (is_ui) {
            jobs.push_back({next.generated_frag_path, stem + ".world.frag", {"FJELL_UI_WORLD"}, {}});
        }
        if (is_sky) {
            jobs.push_back({next.generated_frag_path, stem + ".cubemap.comp", {"FJELL_SKY_CUBEMAP"}, "compute"});
        }

        if (next.has_custom_vertex) {
            auto gen_source = [&](const std::string& vertex_tmpl, const std::string& suffix) -> std::string {
                if (vertex_tmpl.empty()) return {};
                std::string src = generate_vertex_shader(next.metadata, vertex_tmpl);
                std::string path = generated_dir_ + "/" + stem + suffix;
                std::ofstream out(path);
                if (!out.is_open()) return {};
                out << src;
                out.close();
                return path;
            };

            auto add_job = [&](const std::string& vertex_tmpl, const std::string& suffix) {
                std::string path = gen_source(vertex_tmpl, suffix);
                if (!path.empty()) {
                    jobs.push_back({path, stem + suffix, {}, {}});
                }
            };

            add_job(vertex_template_, ".vert");
            add_job(mesh_template_, ".mesh");
            add_job(depth_vertex_template_, ".depth.vert");
            add_job(depth_mesh_template_, ".depth.mesh");
            add_job(shadow_vertex_template_, ".shadow.vert");
            add_job(outline_vertex_template_, ".outline.vert");
        }

        // Compile all shaders in parallel via glslc
        std::vector<std::future<std::string>> futures;
        futures.reserve(jobs.size());
        for (auto& job : jobs) {
            futures.push_back(std::async(std::launch::async,
                [this](std::string src, std::string out_name, std::vector<std::string> defs,
                       std::string stage) {
                    return compile_glsl(src, out_name, defs, stage);
                }, job.source_path, job.output_name, job.defines, job.stage));
        }

        // Collect results
        std::string frag_spv = futures[0].get(); // fragment is always index 0
        size_t idx = 1;
        if (is_ui) {
            next.ui_frag_spv = frag_spv;
            next.ui_world_frag_spv = futures[idx++].get();
        }
        if (is_sky) {
            next.sky_cubemap_spv = futures[idx++].get();
        }

        if (next.has_custom_vertex) {
            auto collect = [&]() -> std::string {
                return (idx < futures.size()) ? futures[idx++].get() : std::string{};
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
        // UiRenderer and TerrainPass own their VkPipelines, built from this
        // entry's SPV.
        if (!owns_own_pipeline) {
            create_pipelines(next, frag_spv);
        }

    } catch (const std::exception& e) {
        // Compilation failed — keep old pipelines, store error
        entry.compile_error = e.what();
        FJELL_GFX_ERROR("Shader compile failed for '{}': {}", entry.fjsl_path, entry.compile_error);

        // Clean up any partially created pipelines in the temp
        destroy_entry_pipelines(next);
        return;
    }

    // Success — destroy old pipelines and swap in new ones
    destroy_entry_pipelines(entry);
    entry.metadata = std::move(next.metadata);
    entry.generated_frag_path = std::move(next.generated_frag_path);
    entry.has_custom_vertex = next.has_custom_vertex;
    entry.opaque = next.opaque;
    entry.transparent = next.transparent;
    entry.gpu_driven = next.gpu_driven;
    entry.skinned_gpu_driven = next.skinned_gpu_driven;
    entry.skinned_opaque = next.skinned_opaque;
    entry.skinned_transparent = next.skinned_transparent;
    entry.outline = next.outline;
    entry.frag_spv = std::move(next.frag_spv);
    entry.ui_frag_spv = std::move(next.ui_frag_spv);
    entry.ui_world_frag_spv = std::move(next.ui_world_frag_spv);
    entry.sky_cubemap_spv = std::move(next.sky_cubemap_spv);
    entry.vert_spv = std::move(next.vert_spv);
    entry.mesh_spv = std::move(next.mesh_spv);
    entry.depth_vert_spv = std::move(next.depth_vert_spv);
    entry.depth_mesh_spv = std::move(next.depth_mesh_spv);
    entry.shadow_vert_spv = std::move(next.shadow_vert_spv);
    entry.outline_vert_spv = std::move(next.outline_vert_spv);
    entry.compile_error.clear();
    ++entry.generation;
}

std::string PipelineCache::compile_glsl(const std::string& source_path,
                                         const std::string& output_name,
                                         const std::vector<std::string>& defines,
                                         const std::string& stage) {
    std::string spv_path = generated_dir_ + "/" + output_name + ".spv";
    std::string include_dir = shader_source_dir_ + "/include";

    // Everything the compiled result is a function of. glslc is
    // deterministic — the same input twice gives byte-identical SPIR-V — so
    // this is an exact key rather than a guess, with none of the staleness a
    // timestamp comparison invites.
    const std::string source = read_file(source_path);
    uint64_t key = include_hash_;
    key = hash_bytes(source, key);
    for (const auto& define : defines) key = hash_bytes(define, key);
    key = hash_bytes(stage, key);
    key = hash_bytes(std::string_view{reinterpret_cast<const char*>(&FJELL_SHADER_CODEGEN_VERSION),
                                      sizeof(FJELL_SHADER_CODEGEN_VERSION)}, key);

    char key_hex[17];
    std::snprintf(key_hex, sizeof(key_hex), "%016llx", static_cast<unsigned long long>(key));
    const std::string cached = spv_cache_dir_ + "/" + key_hex + ".spv";

    // A hit skips glslc entirely: hashing costs about a millisecond against
    // the 64 ms a compile takes.
    if (!source.empty() && std::filesystem::exists(cached)) {
        std::error_code ec;
        std::filesystem::copy_file(cached, spv_path,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (!ec) {
            FJELL_PROFILE_SCOPE_N("glslc_cache_hit");
            return spv_path;
        }
        // A cache we cannot read is not a reason to fail; fall through and
        // compile as if it had never been there.
    }

    // The largest startup cost in the renderer, and until now invisible:
    // ~64 ms per invocation, once per shader, every launch.
    FJELL_PROFILE_SCOPE_N("glslc");
    std::string cmd = "glslc --target-env=vulkan1.3 -I " + include_dir;
    if (!stage.empty()) {
        cmd += " -fshader-stage=" + stage;
    }
    for (const auto& define : defines) {
        cmd += " -D" + define;
    }
    cmd += " " + source_path + " -o " + spv_path + " 2>&1";

    std::string output;
    int status = platform::run_command(cmd, output);
    if (status != 0) {
        FJELL_GFX_ERROR("FJSL shader compilation failed:\n{}", output);
        // Carry what the compiler said, not just which file it was compiling.
        // This is what reaches entry.compile_error and the editor's shader
        // panel; the path alone sent the reader to the log to find out what
        // was actually wrong. #line makes the text name the .fjsl, so it is
        // worth showing.
        throw std::runtime_error(output.empty()
                                     ? "shader compilation failed for: " + source_path
                                     : output);
    }

    // Keep the result for the next run. Written to a temporary and renamed,
    // so a process that dies mid-write leaves no half-file for a later run to
    // read as a valid cached shader. A failure to cache is not a failure to
    // compile: the SPIR-V is already where the caller wants it.
    if (!source.empty()) {
        std::error_code ec;
        const std::string temp = cached + ".tmp";
        std::filesystem::copy_file(spv_path, temp,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (!ec) {
            std::filesystem::rename(temp, cached, ec);
        }
        if (ec) std::filesystem::remove(temp, ec);
    }

    FJELL_GFX_DEBUG("Compiled FJSL shader: {} -> {}", source_path, spv_path);
    return spv_path;
}

void PipelineCache::create_pipelines(Entry& entry, const std::string& spv_path) {
    auto vk = device_.handle();
    auto vert_code = read_spirv(entry.has_custom_vertex && !entry.vert_spv.empty()
        ? entry.vert_spv
        : base_path::resolve("shaders/mesh.vert.spv"));
    auto frag_code = read_spirv(spv_path);

    VkShaderModule vert_module = create_shader_module(vk, vert_code);
    VkShaderModule frag_module = create_shader_module(vk, frag_code);

    // Shared state for all pipeline variants
    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    std::array dynamic_states = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
    dynamic_state.pDynamicStates = dynamic_states.data();

    const auto& rs = entry.metadata.render_state;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    if (rs.cull_disabled) {
        rasterizer.cullMode = VK_CULL_MODE_NONE;
    } else if (rs.cull_front) {
        rasterizer.cullMode = VK_CULL_MODE_FRONT_BIT;
    } else {
        rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    }
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = samples_;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = rs.depth_test_disabled ? VK_FALSE : VK_TRUE;
    depth_stencil.depthWriteEnable = VK_FALSE;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    std::array<VkFormat, 4> color_formats = {color_format_, VK_FORMAT_R8G8_UNORM, OffscreenPass::NORMAL_FORMAT, OffscreenPass::VELOCITY_FORMAT};

    VkPipelineRenderingCreateInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_info.colorAttachmentCount = static_cast<uint32_t>(color_formats.size());
    rendering_info.pColorAttachmentFormats = color_formats.data();
    rendering_info.depthAttachmentFormat = depth_format_;

    // ── Vertex pipeline stages ──
    std::array<VkPipelineShaderStageCreateInfo, 2> vert_stages{};
    vert_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vert_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    vert_stages[0].module = vert_module;
    vert_stages[0].pName = "main";
    vert_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vert_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    vert_stages[1].module = frag_module;
    vert_stages[1].pName = "main";

    auto binding_desc = Vertex::get_binding_description();
    auto attrib_descs = Vertex::get_attribute_descriptions();

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding_desc;
    vertex_input.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrib_descs.size());
    vertex_input.pVertexAttributeDescriptions = attrib_descs.data();

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // ── Opaque pipeline ──
    VkPipelineColorBlendAttachmentState opaque_blend{};
    opaque_blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    opaque_blend.blendEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState opaque_mr_blend{};
    opaque_mr_blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
    opaque_mr_blend.blendEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState opaque_normal_blend{};
    opaque_normal_blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
    opaque_normal_blend.blendEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState opaque_velocity_blend{};
    opaque_velocity_blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
    opaque_velocity_blend.blendEnable = VK_FALSE;

    std::array<VkPipelineColorBlendAttachmentState, 4> opaque_blend_attachments = {opaque_blend, opaque_mr_blend, opaque_normal_blend, opaque_velocity_blend};

    VkPipelineColorBlendStateCreateInfo opaque_blending{};
    opaque_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    opaque_blending.attachmentCount = static_cast<uint32_t>(opaque_blend_attachments.size());
    opaque_blending.pAttachments = opaque_blend_attachments.data();

    VkGraphicsPipelineCreateInfo pipeline_ci{};
    pipeline_ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_ci.pNext = &rendering_info;
    pipeline_ci.stageCount = static_cast<uint32_t>(vert_stages.size());
    pipeline_ci.pStages = vert_stages.data();
    pipeline_ci.pVertexInputState = &vertex_input;
    pipeline_ci.pInputAssemblyState = &input_assembly;
    pipeline_ci.pViewportState = &viewport_state;
    pipeline_ci.pRasterizationState = &rasterizer;
    pipeline_ci.pMultisampleState = &multisampling;
    pipeline_ci.pDepthStencilState = &depth_stencil;
    pipeline_ci.pColorBlendState = &opaque_blending;
    pipeline_ci.pDynamicState = &dynamic_state;
    pipeline_ci.layout = layout_;
    pipeline_ci.renderPass = VK_NULL_HANDLE;

    if (vkCreateGraphicsPipelines(vk, vk_cache_, 1, &pipeline_ci, nullptr, &entry.opaque) != VK_SUCCESS) {
        vkDestroyShaderModule(vk, vert_module, nullptr);
        vkDestroyShaderModule(vk, frag_module, nullptr);
        throw std::runtime_error("Failed to create opaque pipeline for: " + entry.fjsl_path);
    }

    // ── Transparent pipeline ──
    VkPipelineColorBlendAttachmentState trans_blend{};
    trans_blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    trans_blend.blendEnable = VK_TRUE;

    switch (rs.blend) {
        case BlendMode::Add:
            trans_blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            trans_blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            trans_blend.colorBlendOp = VK_BLEND_OP_ADD;
            trans_blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            trans_blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            trans_blend.alphaBlendOp = VK_BLEND_OP_ADD;
            break;
        case BlendMode::Multiply:
            trans_blend.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
            trans_blend.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
            trans_blend.colorBlendOp = VK_BLEND_OP_ADD;
            trans_blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;
            trans_blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            trans_blend.alphaBlendOp = VK_BLEND_OP_ADD;
            break;
        default: // Alpha
            trans_blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            trans_blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            trans_blend.colorBlendOp = VK_BLEND_OP_ADD;
            trans_blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            trans_blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            trans_blend.alphaBlendOp = VK_BLEND_OP_ADD;
            break;
    }

    VkPipelineColorBlendAttachmentState trans_mr_blend{};
    trans_mr_blend.colorWriteMask = 0; // Transparent objects don't write MR
    trans_mr_blend.blendEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState trans_normal_blend{};
    trans_normal_blend.colorWriteMask = 0;
    trans_normal_blend.blendEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState trans_velocity_blend{};
    trans_velocity_blend.colorWriteMask = 0;
    trans_velocity_blend.blendEnable = VK_FALSE;

    std::array<VkPipelineColorBlendAttachmentState, 4> trans_blend_attachments = {trans_blend, trans_mr_blend, trans_normal_blend, trans_velocity_blend};

    VkPipelineColorBlendStateCreateInfo trans_blending{};
    trans_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    trans_blending.attachmentCount = static_cast<uint32_t>(trans_blend_attachments.size());
    trans_blending.pAttachments = trans_blend_attachments.data();

    VkPipelineDepthStencilStateCreateInfo trans_depth{};
    trans_depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    trans_depth.depthTestEnable = rs.depth_test_disabled ? VK_FALSE : VK_TRUE;
    trans_depth.depthWriteEnable = VK_FALSE;
    trans_depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    pipeline_ci.pColorBlendState = &trans_blending;
    pipeline_ci.pDepthStencilState = &trans_depth;

    if (vkCreateGraphicsPipelines(vk, vk_cache_, 1, &pipeline_ci, nullptr, &entry.transparent) != VK_SUCCESS) {
        vkDestroyShaderModule(vk, vert_module, nullptr);
        vkDestroyShaderModule(vk, frag_module, nullptr);
        throw std::runtime_error("Failed to create transparent pipeline for: " + entry.fjsl_path);
    }

    // ── Skinned opaque pipeline ──
    auto skinned_vert_code = read_spirv(base_path::resolve("shaders/skinned_mesh.vert.spv"));
    VkShaderModule skinned_vert_module = create_shader_module(vk, skinned_vert_code);

    auto skinned_binding_desc = SkinnedVertex::get_binding_description();
    auto skinned_attrib_descs = SkinnedVertex::get_attribute_descriptions();

    VkPipelineVertexInputStateCreateInfo skinned_vertex_input{};
    skinned_vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    skinned_vertex_input.vertexBindingDescriptionCount = 1;
    skinned_vertex_input.pVertexBindingDescriptions = &skinned_binding_desc;
    skinned_vertex_input.vertexAttributeDescriptionCount = static_cast<uint32_t>(skinned_attrib_descs.size());
    skinned_vertex_input.pVertexAttributeDescriptions = skinned_attrib_descs.data();

    std::array<VkPipelineShaderStageCreateInfo, 2> skinned_stages{};
    skinned_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    skinned_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    skinned_stages[0].module = skinned_vert_module;
    skinned_stages[0].pName = "main";
    skinned_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    skinned_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    skinned_stages[1].module = frag_module;
    skinned_stages[1].pName = "main";

    pipeline_ci.pColorBlendState = &opaque_blending;
    pipeline_ci.pDepthStencilState = &depth_stencil;
    pipeline_ci.stageCount = static_cast<uint32_t>(skinned_stages.size());
    pipeline_ci.pStages = skinned_stages.data();
    pipeline_ci.pVertexInputState = &skinned_vertex_input;
    pipeline_ci.pInputAssemblyState = &input_assembly;
    pipeline_ci.layout = layout_;

    // Disable backface culling for skinned meshes — bone transforms at
    // joints can flip triangle winding, causing holes in the mesh.
    rasterizer.cullMode = VK_CULL_MODE_NONE;

    if (vkCreateGraphicsPipelines(vk, vk_cache_, 1, &pipeline_ci, nullptr, &entry.skinned_opaque) != VK_SUCCESS) {
        vkDestroyShaderModule(vk, skinned_vert_module, nullptr);
        vkDestroyShaderModule(vk, vert_module, nullptr);
        vkDestroyShaderModule(vk, frag_module, nullptr);
        throw std::runtime_error("Failed to create skinned opaque pipeline for: " + entry.fjsl_path);
    }

    // ── Skinned transparent pipeline ──
    pipeline_ci.pColorBlendState = &trans_blending;
    pipeline_ci.pDepthStencilState = &trans_depth;

    if (vkCreateGraphicsPipelines(vk, vk_cache_, 1, &pipeline_ci, nullptr, &entry.skinned_transparent) != VK_SUCCESS) {
        vkDestroyShaderModule(vk, skinned_vert_module, nullptr);
        vkDestroyShaderModule(vk, vert_module, nullptr);
        vkDestroyShaderModule(vk, frag_module, nullptr);
        throw std::runtime_error("Failed to create skinned transparent pipeline for: " + entry.fjsl_path);
    }

    vkDestroyShaderModule(vk, skinned_vert_module, nullptr);

    // ── GPU-driven mesh shader pipeline (if mesh shaders available) ──
    if (gpu_driven_layout_ != VK_NULL_HANDLE) {
        auto gpu_task_code = read_spirv(base_path::resolve("shaders/mesh_gpu_driven.task.spv"));
        auto gpu_mesh_code = read_spirv(entry.has_custom_vertex && !entry.mesh_spv.empty()
            ? entry.mesh_spv
            : base_path::resolve("shaders/mesh_gpu_driven.mesh.spv"));

        VkShaderModule gpu_task_module = create_shader_module(vk, gpu_task_code);
        VkShaderModule gpu_mesh_module = create_shader_module(vk, gpu_mesh_code);

        std::array<VkPipelineShaderStageCreateInfo, 3> mesh_stages{};
        mesh_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        mesh_stages[0].stage = VK_SHADER_STAGE_TASK_BIT_EXT;
        mesh_stages[0].module = gpu_task_module;
        mesh_stages[0].pName = "main";
        mesh_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        mesh_stages[1].stage = VK_SHADER_STAGE_MESH_BIT_EXT;
        mesh_stages[1].module = gpu_mesh_module;
        mesh_stages[1].pName = "main";
        mesh_stages[2].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        mesh_stages[2].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        mesh_stages[2].module = frag_module;
        mesh_stages[2].pName = "main";

        pipeline_ci.pColorBlendState = &opaque_blending;
        pipeline_ci.pDepthStencilState = &depth_stencil;
        pipeline_ci.stageCount = static_cast<uint32_t>(mesh_stages.size());
        pipeline_ci.pStages = mesh_stages.data();
        pipeline_ci.pVertexInputState = nullptr;
        pipeline_ci.pInputAssemblyState = nullptr;
        pipeline_ci.layout = gpu_driven_layout_;

        if (vkCreateGraphicsPipelines(vk, vk_cache_, 1, &pipeline_ci, nullptr, &entry.gpu_driven) != VK_SUCCESS) {
            vkDestroyShaderModule(vk, gpu_task_module, nullptr);
            vkDestroyShaderModule(vk, gpu_mesh_module, nullptr);
            vkDestroyShaderModule(vk, vert_module, nullptr);
            vkDestroyShaderModule(vk, frag_module, nullptr);
            throw std::runtime_error("Failed to create GPU-driven pipeline for: " + entry.fjsl_path);
        }

        vkDestroyShaderModule(vk, gpu_task_module, nullptr);
        vkDestroyShaderModule(vk, gpu_mesh_module, nullptr);

        // Skinned variant: same layout, same fragment, swap in the
        // skinned mesh + task SPV. FJSL vertex hooks aren't supported on
        // the skinned path yet, so we always use the stock skinned mesh
        // shader regardless of has_custom_vertex.
        auto skin_task_code = read_spirv(base_path::resolve("shaders/skinned_mesh_gpu_driven.task.spv"));
        auto skin_mesh_code = read_spirv(base_path::resolve("shaders/skinned_mesh_gpu_driven.mesh.spv"));

        VkShaderModule skin_task_module = create_shader_module(vk, skin_task_code);
        VkShaderModule skin_mesh_module = create_shader_module(vk, skin_mesh_code);

        std::array<VkPipelineShaderStageCreateInfo, 3> skin_stages{};
        skin_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        skin_stages[0].stage = VK_SHADER_STAGE_TASK_BIT_EXT;
        skin_stages[0].module = skin_task_module;
        skin_stages[0].pName = "main";
        skin_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        skin_stages[1].stage = VK_SHADER_STAGE_MESH_BIT_EXT;
        skin_stages[1].module = skin_mesh_module;
        skin_stages[1].pName = "main";
        skin_stages[2].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        skin_stages[2].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        skin_stages[2].module = frag_module;
        skin_stages[2].pName = "main";

        pipeline_ci.stageCount = static_cast<uint32_t>(skin_stages.size());
        pipeline_ci.pStages = skin_stages.data();

        if (vkCreateGraphicsPipelines(vk, vk_cache_, 1, &pipeline_ci, nullptr, &entry.skinned_gpu_driven) != VK_SUCCESS) {
            vkDestroyShaderModule(vk, skin_task_module, nullptr);
            vkDestroyShaderModule(vk, skin_mesh_module, nullptr);
            vkDestroyShaderModule(vk, vert_module, nullptr);
            vkDestroyShaderModule(vk, frag_module, nullptr);
            throw std::runtime_error("Failed to create skinned GPU-driven pipeline for: " + entry.fjsl_path);
        }

        vkDestroyShaderModule(vk, skin_task_module, nullptr);
        vkDestroyShaderModule(vk, skin_mesh_module, nullptr);
    }

    // ── Outline pipeline ──
    {
        auto outline_vert_code = read_spirv(entry.has_custom_vertex && !entry.outline_vert_spv.empty()
            ? entry.outline_vert_spv
            : base_path::resolve("shaders/outline.vert.spv"));

        VkShaderModule outline_vert_mod = create_shader_module(vk, outline_vert_code);
        auto outline_frag_code = read_spirv(base_path::resolve("shaders/outline.frag.spv"));
        VkShaderModule outline_frag_mod = create_shader_module(vk, outline_frag_code);

        std::array<VkPipelineShaderStageCreateInfo, 2> outline_stages{};
        outline_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        outline_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        outline_stages[0].module = outline_vert_mod;
        outline_stages[0].pName = "main";
        outline_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        outline_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        outline_stages[1].module = outline_frag_mod;
        outline_stages[1].pName = "main";

        // Outline: cull front, alpha blend, no depth write
        rasterizer.cullMode = VK_CULL_MODE_FRONT_BIT;

        VkPipelineColorBlendAttachmentState outline_blend{};
        outline_blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                       VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        outline_blend.blendEnable = VK_TRUE;
        outline_blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        outline_blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        outline_blend.colorBlendOp = VK_BLEND_OP_ADD;
        outline_blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        outline_blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        outline_blend.alphaBlendOp = VK_BLEND_OP_ADD;

        VkPipelineColorBlendAttachmentState outline_mr{};
        outline_mr.colorWriteMask = 0;
        outline_mr.blendEnable = VK_FALSE;

        VkPipelineColorBlendAttachmentState outline_normal{};
        outline_normal.colorWriteMask = 0;
        outline_normal.blendEnable = VK_FALSE;

        VkPipelineColorBlendAttachmentState outline_velocity{};
        outline_velocity.colorWriteMask = 0;
        outline_velocity.blendEnable = VK_FALSE;

        std::array outline_blend_attachments = {outline_blend, outline_mr, outline_normal, outline_velocity};

        VkPipelineColorBlendStateCreateInfo outline_blending{};
        outline_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        outline_blending.attachmentCount = static_cast<uint32_t>(outline_blend_attachments.size());
        outline_blending.pAttachments = outline_blend_attachments.data();

        pipeline_ci.stageCount = static_cast<uint32_t>(outline_stages.size());
        pipeline_ci.pStages = outline_stages.data();
        pipeline_ci.pVertexInputState = &vertex_input;
        pipeline_ci.pInputAssemblyState = &input_assembly;
        pipeline_ci.pColorBlendState = &outline_blending;
        pipeline_ci.pDepthStencilState = &depth_stencil;
        pipeline_ci.layout = outline_layout_;

        if (vkCreateGraphicsPipelines(vk, vk_cache_, 1, &pipeline_ci, nullptr, &entry.outline) != VK_SUCCESS) {
            vkDestroyShaderModule(vk, outline_vert_mod, nullptr);
            vkDestroyShaderModule(vk, outline_frag_mod, nullptr);
            throw std::runtime_error("Failed to create outline pipeline for: " + entry.fjsl_path);
        }

        vkDestroyShaderModule(vk, outline_vert_mod, nullptr);
        vkDestroyShaderModule(vk, outline_frag_mod, nullptr);
    }

    vkDestroyShaderModule(vk, frag_module, nullptr);
    vkDestroyShaderModule(vk, vert_module, nullptr);
}

void PipelineCache::destroy_entry_pipelines(Entry& entry) {
    auto vk = device_.handle();
    if (entry.opaque != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk, entry.opaque, nullptr);
        entry.opaque = VK_NULL_HANDLE;
    }
    if (entry.transparent != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk, entry.transparent, nullptr);
        entry.transparent = VK_NULL_HANDLE;
    }
    if (entry.gpu_driven != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk, entry.gpu_driven, nullptr);
        entry.gpu_driven = VK_NULL_HANDLE;
    }
    if (entry.skinned_gpu_driven != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk, entry.skinned_gpu_driven, nullptr);
        entry.skinned_gpu_driven = VK_NULL_HANDLE;
    }
    if (entry.skinned_opaque != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk, entry.skinned_opaque, nullptr);
        entry.skinned_opaque = VK_NULL_HANDLE;
    }
    if (entry.skinned_transparent != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk, entry.skinned_transparent, nullptr);
        entry.skinned_transparent = VK_NULL_HANDLE;
    }
    if (entry.outline != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk, entry.outline, nullptr);
        entry.outline = VK_NULL_HANDLE;
    }
}

void PipelineCache::reload(uint32_t id) {
    if (id >= entries_.size()) return;
    vkDeviceWaitIdle(device_.handle());
    compile_entry(entries_[id]);
    FJELL_GFX_INFO("Reloaded FJSL pipeline {}", id);
}

namespace {

// Whether a registered shader path and a path from the file watcher name the
// same file. The registered one may be engine-relative ("shaders/toon.fjsl")
// while the watcher always reports absolute, so the test is whether one ends
// with the other — anchored to a separator, so "water.fjsl" does not match a
// project's "deep_water.fjsl".
bool same_shader_file(std::string_view registered, std::string_view changed) {
    if (registered == changed) return true;
    if (registered.size() >= changed.size()) return false;
    if (!changed.ends_with(registered)) return false;
    return changed[changed.size() - registered.size() - 1] == '/';
}

} // namespace

uint32_t PipelineCache::reload_changed(const std::vector<std::string>& changed_paths) {
    if (changed_paths.empty()) return 0;

    std::vector<uint32_t> to_reload;
    for (uint32_t id = 0; id < entries_.size(); ++id) {
        for (const auto& changed : changed_paths) {
            if (!same_shader_file(entries_[id].fjsl_path, changed)) continue;
            to_reload.push_back(id);
            break;
        }
    }
    if (to_reload.empty()) return 0;

    // One wait for the whole batch rather than one per pipeline: every entry
    // here is about to have its old pipelines destroyed.
    vkDeviceWaitIdle(device_.handle());
    for (uint32_t id : to_reload) {
        compile_entry(entries_[id]);
        FJELL_GFX_INFO("Reloaded FJSL pipeline {} ('{}')", id, entries_[id].fjsl_path);
    }
    return static_cast<uint32_t>(to_reload.size());
}

void PipelineCache::recreate_all() {
    vkDeviceWaitIdle(device_.handle());
    int failures = 0;
    for (auto& entry : entries_) {
        compile_entry(entry);
        if (!entry.compile_error.empty()) failures++;
    }
    if (failures > 0) {
        FJELL_GFX_WARN("Recreated FJSL pipelines: {}/{} failed", failures, entries_.size());
    } else {
        FJELL_GFX_INFO("Recreated all {} FJSL pipelines", entries_.size());
    }
}

VkPipeline PipelineCache::opaque_pipeline(uint32_t id) const {
    auto p = entries_[id].opaque;
    return p != VK_NULL_HANDLE ? p : entries_[0].opaque;
}

VkPipeline PipelineCache::transparent_pipeline(uint32_t id) const {
    auto p = entries_[id].transparent;
    return p != VK_NULL_HANDLE ? p : entries_[0].transparent;
}

VkPipeline PipelineCache::gpu_driven_pipeline(uint32_t id) const {
    auto p = entries_[id].gpu_driven;
    return p != VK_NULL_HANDLE ? p : entries_[0].gpu_driven;
}

VkPipeline PipelineCache::skinned_gpu_driven_pipeline(uint32_t id) const {
    auto p = entries_[id].skinned_gpu_driven;
    return p != VK_NULL_HANDLE ? p : entries_[0].skinned_gpu_driven;
}

VkPipeline PipelineCache::skinned_opaque_pipeline(uint32_t id) const {
    auto p = entries_[id].skinned_opaque;
    return p != VK_NULL_HANDLE ? p : entries_[0].skinned_opaque;
}

VkPipeline PipelineCache::skinned_transparent_pipeline(uint32_t id) const {
    auto p = entries_[id].skinned_transparent;
    return p != VK_NULL_HANDLE ? p : entries_[0].skinned_transparent;
}

VkPipeline PipelineCache::outline_pipeline(uint32_t id) const {
    auto p = entries_[id].outline;
    return p != VK_NULL_HANDLE ? p : entries_[0].outline;
}

const ShaderMetadata& PipelineCache::metadata(uint32_t id) const {
    return entries_[id].metadata;
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

const std::string& PipelineCache::outline_vert_spv(uint32_t id) const {
    return entries_[id].outline_vert_spv;
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

} // namespace fjell
