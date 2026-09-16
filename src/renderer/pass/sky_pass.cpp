#include "renderer/pass/sky_pass.hpp"
#include "renderer/viewport_source.hpp"
#include "core/profiler.hpp"
#include "renderer/bindless.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/gpu_context.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass/pass_gpu_zone.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/resources/shader_utils.hpp"
#include "core/project.hpp"
#include "environment/environment_state.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#include <array>
namespace fjell {

void SkyPass::prepare_frame(const FrameContext& ctx, GpuContext& /*gpu*/) {
    if (ctx.environment == nullptr) return;

    const auto& sky = ctx.environment->sky;
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
    }
}

Result<> SkyPass::create(Device& device, const PassResources& resources) {
    device_ = device.handle();
    vk_pipeline_cache_ = resources.pipeline_cache;
    cached_resources_ = resources;

    auto vert_code = read_spirv(base_path::resolve("shaders/sky.vert.spv"));
    auto frag_code = read_spirv(base_path::resolve("shaders/sky.frag.spv"));

    auto vert_module = create_shader_module(device_, vert_code);
    auto frag_module = create_shader_module(device_, frag_code);

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert_module;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag_module;
    stages[1].pName = "main";

    // No vertex input — fullscreen triangle
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    std::array dynamic_states = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
    dynamic_state.pDynamicStates = dynamic_states.data();

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = resources.sample_count;

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendAttachmentState mr_blend{};
    mr_blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
    mr_blend.blendEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState normal_blend{};
    normal_blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
    normal_blend.blendEnable = VK_FALSE;

    std::array blend_attachments = {blend, mr_blend, normal_blend};

    VkPipelineColorBlendStateCreateInfo color_blending{};
    color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blending.attachmentCount = static_cast<uint32_t>(blend_attachments.size());
    color_blending.pAttachments = blend_attachments.data();

    // Depth test: only draw where depth == 1.0 (clear value, no geometry)
    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_FALSE;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    // Push constants: top_color (vec4) + bottom_color (vec4) + viewport_height + padding
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push_range.offset = 0;
    push_range.size = 80; // 2 * vec4 + 12 floats (sky_mode + atmosphere params + padding)

    std::array set_layouts = {resources.global_set_layout};

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = static_cast<uint32_t>(set_layouts.size());
    layout_info.pSetLayouts = set_layouts.data();
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;

    if (vkCreatePipelineLayout(device_, &layout_info, nullptr, &layout_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, vert_module, nullptr);
        vkDestroyShaderModule(device_, frag_module, nullptr);
        return make_error("Failed to create sky pipeline layout");
    }

    VkFormat depth_format = resources.depth_format;
    std::array<VkFormat, 3> color_formats = {resources.color_format, VK_FORMAT_R8G8_UNORM,
                                             OffscreenPass::NORMAL_FORMAT};

    VkPipelineRenderingCreateInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_info.colorAttachmentCount = static_cast<uint32_t>(color_formats.size());
    rendering_info.pColorAttachmentFormats = color_formats.data();
    rendering_info.depthAttachmentFormat = depth_format;

    VkGraphicsPipelineCreateInfo pipeline_ci{};
    pipeline_ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_ci.pNext = &rendering_info;
    pipeline_ci.stageCount = static_cast<uint32_t>(stages.size());
    pipeline_ci.pStages = stages.data();
    pipeline_ci.pVertexInputState = &vertex_input;
    pipeline_ci.pInputAssemblyState = &input_assembly;
    pipeline_ci.pViewportState = &viewport_state;
    pipeline_ci.pRasterizationState = &rasterizer;
    pipeline_ci.pMultisampleState = &multisampling;
    pipeline_ci.pDepthStencilState = &depth_stencil;
    pipeline_ci.pColorBlendState = &color_blending;
    pipeline_ci.pDynamicState = &dynamic_state;
    pipeline_ci.layout = layout_;
    pipeline_ci.renderPass = VK_NULL_HANDLE;

    if (vkCreateGraphicsPipelines(device_, vk_pipeline_cache_, 1, &pipeline_ci,
                                   nullptr, &pipeline_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, vert_module, nullptr);
        vkDestroyShaderModule(device_, frag_module, nullptr);
        return make_error("Failed to create sky pipeline");
    }

    vkDestroyShaderModule(device_, frag_module, nullptr);
    vkDestroyShaderModule(device_, vert_module, nullptr);

    FJELL_GFX_DEBUG("Sky pass created");
    return {};
}

void SkyPass::post_create(GpuContext& gpu, PassRegistry& /*passes*/) {
    fjsl_pipeline_cache_ = gpu.pipeline_cache();
    fjsl_bindless_ = &gpu.bindless_textures();
}

void SkyPass::destroy() {
    destroy_fjsl_pipeline();
    if (pipeline_ != VK_NULL_HANDLE) { vkDestroyPipeline(device_, pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
    if (layout_ != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device_, layout_, nullptr); layout_ = VK_NULL_HANDLE; }
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
    builder.write(msaa_color, ResourceAccess::color_attachment);
    builder.write(msaa_mr, ResourceAccess::color_attachment);
    builder.write(msaa_normal, ResourceAccess::color_attachment);
    // Depth-tested so the sky only fills pixels no geometry reached, but
    // never written.
    builder.read(msaa_depth, ResourceAccess::depth_attachment_read);
    // The backward cull walk starts at tonemap and follows readers of the
    // resolved `color`. With MSAA on, `msaa_color` is a separate image
    // nothing samples, so without this anchor the sky is culled entirely.
    builder.has_side_effects();
}

void SkyPass::begin_rendering(const FrameContext& ctx) {
    auto* target = ctx.target;

    std::array<VkRenderingAttachmentInfo, 3> color_attachments{};
    const std::array<VkImageView, 3> views = {
        target->msaa_color_view(), target->msaa_mr_view(), target->msaa_normal_view()};
    for (size_t i = 0; i < color_attachments.size(); ++i) {
        color_attachments[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color_attachments[i].imageView = views[i];
        color_attachments[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color_attachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }

    VkRenderingAttachmentInfo depth_attachment{};
    depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth_attachment.imageView = target->msaa_depth_view();
    depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.extent = target->extent();
    ri.layerCount = 1;
    ri.colorAttachmentCount = static_cast<uint32_t>(color_attachments.size());
    ri.pColorAttachments = color_attachments.data();
    ri.pDepthAttachment = &depth_attachment;

    vkCmdBeginRendering(ctx.cmd, &ri);

    VkViewport vp{};
    vp.width = static_cast<float>(target->extent().width);
    vp.height = static_cast<float>(target->extent().height);
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(ctx.cmd, 0, 1, &vp);

    VkRect2D sc{};
    sc.extent = target->extent();
    vkCmdSetScissor(ctx.cmd, 0, 1, &sc);
}

void SkyPass::record(const FrameContext& ctx) {
    if (!ctx.target) return;

    FJELL_PROFILE_SCOPE_N("record_sky");
    FJELL_GPU_ZONE(ctx, "Sky");

    begin_rendering(ctx);

    // A backdrop replaces whatever sky is configured, custom shader included;
    // the sky cubemap the surfaces reflect is built by SkyCubemapPass and
    // untouched by this.
    const bool has_backdrop = ctx.backdrop != nullptr;
    if (!has_backdrop && mode_ == Mode::Fjsl && fjsl_pipeline_ != VK_NULL_HANDLE) {
        struct {
            float viewport_width;
            float viewport_height;
            uint32_t sky_material_id;
            uint32_t pad;
        } push{
            static_cast<float>(ctx.target->extent().width),
            static_cast<float>(ctx.target->extent().height),
            fjsl_material_id_,
            0u,
        };

        std::array sets = {ctx.global_descriptor_set, ctx.bindless_set};
        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fjsl_pipeline_);
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fjsl_layout_,
                                0, static_cast<uint32_t>(sets.size()), sets.data(),
                                0, nullptr);
        vkCmdPushConstants(ctx.cmd, fjsl_layout_, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(push), &push);
        vkCmdDraw(ctx.cmd, 3, 1, 0, 0);
        vkCmdEndRendering(ctx.cmd);
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
        static_cast<float>(ctx.target->extent().width),
        static_cast<float>(ctx.target->extent().height),
        static_cast<float>(has_backdrop ? Mode::Gradient : mode_),
        atm_rayleigh_scale_,
        atm_mie_scale_,
        atm_mie_g_,
        atm_sun_intensity_,
        atm_mie_coeff_,
        atm_hdri_floor_,
        has_hdri_ ? 1.0f : 0.0f
    };

    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_,
                            0, 1, &ctx.global_descriptor_set, 0, nullptr);
    vkCmdPushConstants(ctx.cmd, layout_, VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(push), &push);
    vkCmdDraw(ctx.cmd, 3, 1, 0, 0);

    vkCmdEndRendering(ctx.cmd);
}

void SkyPass::ensure_fjsl_pipeline(const PassResources& resources) {
    if (!fjsl_pipeline_cache_ || !fjsl_bindless_) {
        return;  // wiring not provided — silently skip; gradient fallback runs.
    }
    if (sky_shader_ == fjsl_pipeline_shader_ && fjsl_pipeline_ != VK_NULL_HANDLE &&
        fjsl_pipeline_cache_->generation(fjsl_pipeline_id_) == fjsl_pipeline_generation_) {
        return;  // already built for this path and this compile of it.
    }

    destroy_fjsl_pipeline();

    // The inspector stores project-relative paths (e.g. "../Fjell/shaders/foo.fjsl"
    // when the engine shader lives outside the project tree). Resolve to an
    // absolute path before handing to PipelineCache, which otherwise treats
    // strings as engine-relative and would double-prefix the "../" form.
    std::string canonical = sky_shader_;
    if (canonical.find("..") != std::string::npos || canonical.starts_with("/")) {
        std::string resolved = base_path::resolve_asset(sky_shader_, project_);
        std::error_code ec;
        auto abs = std::filesystem::weakly_canonical(resolved, ec);
        canonical = ec ? resolved : abs.string();
    }

    // Compile the shader through PipelineCache (parses metadata, runs
    // glslc, doesn't create any spatial pipelines for shader_type sky).
    uint32_t pid = fjsl_pipeline_cache_->register_shader(canonical);
    if (fjsl_pipeline_cache_->has_compile_error(pid)) {
        FJELL_GFX_ERROR("SkyPass: FJSL '{}' failed to compile: {}",
                        sky_shader_, fjsl_pipeline_cache_->compile_error(pid));
        return;
    }
    const auto& meta = fjsl_pipeline_cache_->metadata(pid);
    if (meta.shader_type != "sky") {
        FJELL_GFX_ERROR("SkyPass: '{}' is shader_type '{}', expected 'sky'",
                        sky_shader_, meta.shader_type);
        return;
    }
    const std::string& frag_spv = fjsl_pipeline_cache_->frag_spv_path(pid);

    // Allocate a material SSBO row + params slice for this sky.
    GpuMaterial gpu{};
    if (meta.custom_param_count > 0) {
        gpu.param_offset = fjsl_bindless_->allocate_params(meta.custom_param_count);
        gpu.param_count  = meta.custom_param_count;

        // Seed defaults from the parsed uniforms.
        std::vector<float> seed(meta.custom_param_count, 0.0f);
        for (const auto& u : meta.uniforms) {
            if (u.type == UniformType::Sampler2D) continue;
            uint32_t count = uniform_float_count(u.type);
            for (uint32_t i = 0; i < count && i < u.default_value.size(); ++i) {
                seed[u.slot + i] = u.default_value[i];
            }
            encode_uniform_for_gpu(u, std::span{seed}.subspan(u.slot, count));
        }
        fjsl_bindless_->write_params(gpu.param_offset, seed.data(), meta.custom_param_count);
    }
    fjsl_material_id_ = fjsl_bindless_->add_material(gpu);
    fjsl_pipeline_id_ = pid;
    fjsl_pipeline_generation_ = fjsl_pipeline_cache_->generation(pid);

    // Build the pipeline layout: set 0 = global UBO/cubemap, set 1 = bindless.
    std::array set_layouts = {
        resources.global_set_layout,
        resources.material_set_layout,
    };

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(float) * 2 + sizeof(uint32_t) * 2;

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = static_cast<uint32_t>(set_layouts.size());
    layout_info.pSetLayouts = set_layouts.data();
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;
    if (vkCreatePipelineLayout(device_, &layout_info, nullptr, &fjsl_layout_) != VK_SUCCESS) {
        FJELL_GFX_ERROR("SkyPass: failed to create FJSL pipeline layout");
        return;
    }

    // Reuse the built-in sky.vert (fullscreen triangle); only the fragment is FJSL.
    auto vert_code = read_spirv(base_path::resolve("shaders/sky.vert.spv"));
    auto frag_code = read_spirv(frag_spv);
    auto vert_module = create_shader_module(device_, vert_code);
    auto frag_module = create_shader_module(device_, frag_code);

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert_module;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag_module;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    std::array dynamic_states = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
    dynamic_state.pDynamicStates = dynamic_states.data();

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = resources.sample_count;

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendAttachmentState mr_blend{};
    mr_blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
    VkPipelineColorBlendAttachmentState normal_blend{};
    normal_blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
    std::array blend_attachments = {blend, mr_blend, normal_blend};

    VkPipelineColorBlendStateCreateInfo color_blending{};
    color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blending.attachmentCount = static_cast<uint32_t>(blend_attachments.size());
    color_blending.pAttachments = blend_attachments.data();

    // Same depth setup as built-in sky: pass where nothing has been drawn yet.
    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_FALSE;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    std::array<VkFormat, 3> color_formats = {
        resources.color_format, VK_FORMAT_R8G8_UNORM,
        OffscreenPass::NORMAL_FORMAT
    };
    VkPipelineRenderingCreateInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_info.colorAttachmentCount = static_cast<uint32_t>(color_formats.size());
    rendering_info.pColorAttachmentFormats = color_formats.data();
    rendering_info.depthAttachmentFormat = resources.depth_format;

    VkGraphicsPipelineCreateInfo pipeline_ci{};
    pipeline_ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_ci.pNext = &rendering_info;
    pipeline_ci.stageCount = static_cast<uint32_t>(stages.size());
    pipeline_ci.pStages = stages.data();
    pipeline_ci.pVertexInputState = &vertex_input;
    pipeline_ci.pInputAssemblyState = &input_assembly;
    pipeline_ci.pViewportState = &viewport_state;
    pipeline_ci.pRasterizationState = &rasterizer;
    pipeline_ci.pMultisampleState = &multisampling;
    pipeline_ci.pDepthStencilState = &depth_stencil;
    pipeline_ci.pColorBlendState = &color_blending;
    pipeline_ci.pDynamicState = &dynamic_state;
    pipeline_ci.layout = fjsl_layout_;
    pipeline_ci.renderPass = VK_NULL_HANDLE;

    if (vkCreateGraphicsPipelines(device_, vk_pipeline_cache_, 1, &pipeline_ci,
                                  nullptr, &fjsl_pipeline_) != VK_SUCCESS) {
        FJELL_GFX_ERROR("SkyPass: failed to create FJSL sky pipeline for '{}'", sky_shader_);
        vkDestroyShaderModule(device_, frag_module, nullptr);
        vkDestroyShaderModule(device_, vert_module, nullptr);
        return;
    }

    vkDestroyShaderModule(device_, frag_module, nullptr);
    vkDestroyShaderModule(device_, vert_module, nullptr);

    fjsl_pipeline_shader_ = sky_shader_;
    FJELL_GFX_INFO("SkyPass: built FJSL sky pipeline for '{}' (material_id={})",
                   sky_shader_, fjsl_material_id_);
}

void SkyPass::destroy_fjsl_pipeline() {
    if (fjsl_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, fjsl_pipeline_, nullptr);
        fjsl_pipeline_ = VK_NULL_HANDLE;
    }
    if (fjsl_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, fjsl_layout_, nullptr);
        fjsl_layout_ = VK_NULL_HANDLE;
    }
    fjsl_pipeline_shader_.clear();
    // Note: the material SSBO row and params slice stay allocated. The
    // bindless registry is append-only by design; we don't reclaim them
    // when the sky changes. Worst case: a handful of orphaned floats.
}

} // namespace fjell
