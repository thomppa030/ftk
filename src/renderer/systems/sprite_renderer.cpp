#include "renderer/systems/sprite_renderer.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/resources/texture.hpp"
#include "renderer/gpu/shader_utils.hpp"
#include "renderer/components/sprite_desc.hpp"
#include "renderer/frustum.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "core/project.hpp"
#include "scene/scene.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace fjell {

void SpriteRenderer::create(Device& device, const PassResources& resources) {
    device_ = device.handle();
    vk_pipeline_cache_ = resources.pipeline_cache;
    allocator_ = resources.allocator;
    global_set_layout_ = resources.global_set_layout;
    bindless_set_layout_ = resources.material_set_layout; // bindless textures layout
    color_format_ = resources.color_format;
    depth_format_ = resources.depth_format;
    sample_count_ = resources.sample_count;

    // Sprite SSBO descriptor set layout (set 2, binding 0)
    VkDescriptorSetLayoutBinding ssbo_binding{};
    ssbo_binding.binding = 0;
    ssbo_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ssbo_binding.descriptorCount = 1;
    ssbo_binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_ci.bindingCount = 1;
    layout_ci.pBindings = &ssbo_binding;
    if (vkCreateDescriptorSetLayout(device_, &layout_ci, nullptr, &sprite_set_layout_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create sprite set layout");
    }

    // Pipeline layout: set 0 = global, set 1 = bindless textures, set 2 = sprite SSBO
    VkDescriptorSetLayout sets[] = {global_set_layout_, bindless_set_layout_, sprite_set_layout_};
    VkPipelineLayoutCreateInfo pipe_layout_ci{};
    pipe_layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipe_layout_ci.setLayoutCount = 3;
    pipe_layout_ci.pSetLayouts = sets;
    if (vkCreatePipelineLayout(device_, &pipe_layout_ci, nullptr, &layout_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create sprite pipeline layout");
    }

    // Build all three bucket pipelines. Shared state first, then per-bucket
    // overrides for shader pair, depth-write, and blend.
    auto unlit_vert = create_shader_module(device_, read_spirv(base_path::resolve("shaders/sprite.vert.spv")));
    auto unlit_frag = create_shader_module(device_, read_spirv(base_path::resolve("shaders/sprite.frag.spv")));
    auto lit_vert   = create_shader_module(device_, read_spirv(base_path::resolve("shaders/sprite_lit.vert.spv")));
    auto lit_frag   = create_shader_module(device_, read_spirv(base_path::resolve("shaders/sprite_lit.frag.spv")));

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = 2;
    dynamic_state.pDynamicStates = dynamic_states;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = sample_count_;

    VkFormat color_formats[] = {color_format_, VK_FORMAT_R8G8_UNORM, OffscreenPass::NORMAL_FORMAT, OffscreenPass::VELOCITY_FORMAT};
    VkPipelineRenderingCreateInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_info.colorAttachmentCount = 4;
    rendering_info.pColorAttachmentFormats = color_formats;
    rendering_info.depthAttachmentFormat = depth_format_;

    auto build_pipeline = [&](VkShaderModule vmod, VkShaderModule fmod,
                              bool depth_write, bool alpha_blend,
                              VkPipeline& out) {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vmod;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fmod;
        stages[1].pName = "main";

        VkPipelineDepthStencilStateCreateInfo depth_stencil{};
        depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth_stencil.depthTestEnable = VK_TRUE;
        depth_stencil.depthWriteEnable = depth_write ? VK_TRUE : VK_FALSE;
        depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

        VkPipelineColorBlendAttachmentState color_blend{};
        color_blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        if (alpha_blend) {
            color_blend.blendEnable = VK_TRUE;
            color_blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            color_blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            color_blend.colorBlendOp = VK_BLEND_OP_ADD;
            color_blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            color_blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            color_blend.alphaBlendOp = VK_BLEND_OP_ADD;
        } else {
            color_blend.blendEnable = VK_FALSE;
        }

        // Lit pipelines write the full GBuffer (color, MR, normal); unlit
        // pipelines mask MR/normal/velocity to keep zeroes in the GBuffer
        // (matches the existing unlit behaviour).
        const bool write_gbuffer = (vmod == lit_vert);

        VkPipelineColorBlendAttachmentState mr_blend{};
        mr_blend.colorWriteMask = write_gbuffer
            ? (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT)
            : 0;
        mr_blend.blendEnable = VK_FALSE;

        VkPipelineColorBlendAttachmentState normal_blend{};
        normal_blend.colorWriteMask = write_gbuffer
            ? (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT)
            : 0;
        normal_blend.blendEnable = VK_FALSE;

        VkPipelineColorBlendAttachmentState velocity_blend{};
        velocity_blend.colorWriteMask = 0;
        velocity_blend.blendEnable = VK_FALSE;

        VkPipelineColorBlendAttachmentState blend_attachments[] = {color_blend, mr_blend, normal_blend, velocity_blend};

        VkPipelineColorBlendStateCreateInfo color_blending{};
        color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        color_blending.attachmentCount = 4;
        color_blending.pAttachments = blend_attachments;

        VkGraphicsPipelineCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        ci.pNext = &rendering_info;
        ci.stageCount = 2;
        ci.pStages = stages;
        ci.pVertexInputState = &vertex_input;
        ci.pInputAssemblyState = &input_assembly;
        ci.pViewportState = &viewport_state;
        ci.pRasterizationState = &rasterizer;
        ci.pMultisampleState = &multisampling;
        ci.pDepthStencilState = &depth_stencil;
        ci.pColorBlendState = &color_blending;
        ci.pDynamicState = &dynamic_state;
        ci.layout = layout_;

        if (vkCreateGraphicsPipelines(device_, vk_pipeline_cache_, 1, &ci, nullptr, &out) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create sprite pipeline");
        }
    };

    build_pipeline(unlit_vert, unlit_frag, /*depth_write=*/false, /*alpha_blend=*/true,
                   pipelines_[static_cast<size_t>(Bucket::UnlitTransparent)]);
    build_pipeline(lit_vert,   lit_frag,   /*depth_write=*/false, /*alpha_blend=*/true,
                   pipelines_[static_cast<size_t>(Bucket::LitTransparent)]);
    build_pipeline(lit_vert,   lit_frag,   /*depth_write=*/true,  /*alpha_blend=*/false,
                   pipelines_[static_cast<size_t>(Bucket::LitOpaque)]);

    // Depth-only pipeline: no color attachments, depth write on. Reuses the
    // lit vertex shader and a tiny alpha-discard fragment.
    {
        auto depth_frag = create_shader_module(device_, read_spirv(base_path::resolve("shaders/sprite_depth.frag.spv")));

        VkPipelineShaderStageCreateInfo depth_stages[2]{};
        depth_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        depth_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        depth_stages[0].module = lit_vert;
        depth_stages[0].pName = "main";
        depth_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        depth_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        depth_stages[1].module = depth_frag;
        depth_stages[1].pName = "main";

        VkPipelineDepthStencilStateCreateInfo depth_ds{};
        depth_ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth_ds.depthTestEnable = VK_TRUE;
        depth_ds.depthWriteEnable = VK_TRUE;
        depth_ds.depthCompareOp = VK_COMPARE_OP_LESS;

        VkPipelineColorBlendStateCreateInfo no_blend{};
        no_blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        no_blend.attachmentCount = 0;

        VkPipelineRenderingCreateInfo depth_rendering{};
        depth_rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        depth_rendering.colorAttachmentCount = 0;
        depth_rendering.depthAttachmentFormat = depth_format_;

        VkGraphicsPipelineCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        ci.pNext = &depth_rendering;
        ci.stageCount = 2;
        ci.pStages = depth_stages;
        ci.pVertexInputState = &vertex_input;
        ci.pInputAssemblyState = &input_assembly;
        ci.pViewportState = &viewport_state;
        ci.pRasterizationState = &rasterizer;
        ci.pMultisampleState = &multisampling;
        ci.pDepthStencilState = &depth_ds;
        ci.pColorBlendState = &no_blend;
        ci.pDynamicState = &dynamic_state;
        ci.layout = layout_;

        if (vkCreateGraphicsPipelines(device_, vk_pipeline_cache_, 1, &ci, nullptr, &depth_pipeline_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create sprite depth pipeline");
        }
        vkDestroyShaderModule(device_, depth_frag, nullptr);
    }

    vkDestroyShaderModule(device_, unlit_vert, nullptr);
    vkDestroyShaderModule(device_, unlit_frag, nullptr);
    vkDestroyShaderModule(device_, lit_vert, nullptr);
    vkDestroyShaderModule(device_, lit_frag, nullptr);

    // Descriptor pool for 2 sprite SSBO sets
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = MAX_FRAMES_IN_FLIGHT;

    VkDescriptorPoolCreateInfo desc_pool_ci{};
    desc_pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    desc_pool_ci.maxSets = MAX_FRAMES_IN_FLIGHT;
    desc_pool_ci.poolSizeCount = 1;
    desc_pool_ci.pPoolSizes = &pool_size;
    if (vkCreateDescriptorPool(device_, &desc_pool_ci, nullptr, &pool_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create sprite descriptor pool");
    }

    // Double-buffered SSBOs + descriptor sets
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        sprite_ssbos_[i] = std::make_unique<Buffer>(
            allocator_, SSBO_SIZE,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VMA_MEMORY_USAGE_AUTO, VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
        sprite_ssbos_[i]->map();

        VkDescriptorSetAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool = pool_;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &sprite_set_layout_;
        if (vkAllocateDescriptorSets(device_, &alloc, &sprite_sets_[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to allocate sprite descriptor set");
        }

        VkDescriptorBufferInfo buf_info{sprite_ssbos_[i]->handle(), 0, SSBO_SIZE};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = sprite_sets_[i];
        write.dstBinding = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.descriptorCount = 1;
        write.pBufferInfo = &buf_info;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }

    FJELL_GFX_DEBUG("Sprite renderer created");
}

void SpriteRenderer::destroy() {
    auto safe_destroy = [&](auto& handle, auto destroy_fn) {
        if (handle != VK_NULL_HANDLE) { destroy_fn(device_, handle, nullptr); handle = VK_NULL_HANDLE; }
    };

    for (auto& ssbo : sprite_ssbos_) ssbo.reset();
    safe_destroy(pool_, vkDestroyDescriptorPool);
    for (auto& pipe : pipelines_) safe_destroy(pipe, vkDestroyPipeline);
    safe_destroy(depth_pipeline_, vkDestroyPipeline);
    safe_destroy(layout_, vkDestroyPipelineLayout);
    safe_destroy(sprite_set_layout_, vkDestroyDescriptorSetLayout);
}

void SpriteRenderer::sync_textures(Scene& scene, ResourceManager& resources, const Project* project) {
    FJELL_PROFILE_SCOPE_N("sprite_sync_textures");
    for (auto [obj, sprite_ptr] : scene.view<SpriteComponent>()) {

        auto& sprite = *sprite_ptr;
        if (!sprite.texture_loaded && !sprite.texture_path.empty()) {
            std::string resolved = base_path::resolve_asset(sprite.texture_path, project);
            if (std::filesystem::exists(resolved)) {
                auto tex_result = sprite.pixel_art
                    ? resources.load_texture_nearest(resolved)
                    : resources.load_texture(resolved);
                if (tex_result) {
                    sprite.texture_index = resources.bindless_index(*tex_result);
                    auto* texture = resources.get(*tex_result);
                    if (texture) {
                        sprite.tex_width = texture->width();
                        sprite.tex_height = texture->height();
                    }
                    sprite.texture_loaded = true;
                }
            }
        }

        if (!sprite.normal_loaded && !sprite.normal_path.empty()) {
            std::string resolved = base_path::resolve_asset(sprite.normal_path, project);
            if (std::filesystem::exists(resolved)) {
                auto nm_result = resources.load_texture(resolved);
                if (nm_result) {
                    sprite.normal_index = resources.bindless_index(*nm_result);
                    sprite.normal_loaded = true;
                }
            }
        }
    }
}

void SpriteRenderer::update(const RenderSubsystemContext& ctx) {
    if (ctx.scene) {
        update_impl(*ctx.scene, ctx.view_projection, ctx.camera_position,
                     ctx.frame_index, ctx.playing);
    }
}

void SpriteRenderer::record_depth(const FrameContext& ctx, gpu::RenderEncoder& /*pass*/) {
    FJELL_PROFILE_SCOPE_N("sprite_record_depth");
    const SpriteRun& range = opaque_range_[ctx.frame_index];
    if (range.count == 0) return;

    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, depth_pipeline_);

    VkViewport vp{};
    vp.width = static_cast<float>(ctx.target->extent().width);
    vp.height = static_cast<float>(ctx.target->extent().height);
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(ctx.cmd, 0, 1, &vp);

    VkRect2D sc{};
    sc.extent = ctx.target->extent();
    vkCmdSetScissor(ctx.cmd, 0, 1, &sc);

    VkDescriptorSet sets[] = {ctx.global_descriptor_set, ctx.bindless_set, sprite_sets_[ctx.frame_index]};
    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_,
                            0, 3, sets, 0, nullptr);

    vkCmdDraw(ctx.cmd, range.count * 6, 1, range.first * 6, 0);
}

void SpriteRenderer::record_opaque(const FrameContext& ctx, gpu::RenderEncoder& /*pass*/) {
    record_runs(ctx, std::span(&opaque_range_[ctx.frame_index], 1));
}

void SpriteRenderer::record_transparent(const FrameContext& ctx, gpu::RenderEncoder& /*pass*/) {
    // Unlit sprites and lit billboards blend in one list in layer order,
    // switching pipeline where one kind follows the other.
    record_runs(ctx, transparent_runs_[ctx.frame_index]);
}

void SpriteRenderer::update_impl(const Scene& scene, const glm::mat4& view_projection,
                                  const glm::vec3& camera_position, uint32_t frame_index,
                                  bool playing) {
    FJELL_PROFILE_SCOPE_N("sprite_update");
    Frustum frustum = Frustum::from_vp_matrix(view_projection);

    // Gather visible sprites (reuse persistent scratch vectors)
    auto& sprites = sprites_scratch_;
    auto& sort_entries = sort_scratch_;
    sprites.clear();
    sort_entries.clear();

    for (auto [obj, sp] : scene.view<SpriteComponent>()) {
        if (!sp->texture_loaded) continue;
        if (playing && sp->editor_only) continue;

        const auto& sprite = *sp;
        const auto* spatial = obj->as_spatial();
        if (!spatial) continue;
        glm::vec3 pos = glm::vec3(spatial->world_matrix_[3]);
        glm::vec3 scale = spatial->transform.scale;

        glm::vec2 half_size;
        if (sprite.pixels_per_unit > 0.0f && sprite.tex_width > 0 && sprite.tex_height > 0) {
            // Auto-size from texture dimensions and UV rect sub-region
            float region_w = (sprite.uv_rect.z - sprite.uv_rect.x) * static_cast<float>(sprite.tex_width);
            float region_h = (sprite.uv_rect.w - sprite.uv_rect.y) * static_cast<float>(sprite.tex_height);
            float w = region_w / sprite.pixels_per_unit;
            float h = region_h / sprite.pixels_per_unit;
            half_size = {w * scale.x * 0.5f, h * scale.y * 0.5f};
        } else {
            half_size = {scale.x * 0.5f, scale.y * 0.5f};
        }

        // Frustum cull with a sphere approximation
        float radius = glm::length(glm::vec3(half_size, 0.0f));
        AABB aabb;
        aabb.min = pos - glm::vec3(radius);
        aabb.max = pos + glm::vec3(radius);
        if (!frustum.intersects(aabb)) continue;

        GpuSprite gs{};
        gs.position = pos;
        gs.flags = 0;
        if (sprite.flat)   gs.flags |= 1u;
        if (sprite.flip_h) gs.flags |= 2u;
        if (sprite.lit)    gs.flags |= 4u;
        gs.size = half_size;
        gs.texture_index = sprite.texture_index;
        gs.opacity = sprite.opacity;
        gs.tint = sprite.tint;
        gs.uv_rect = sprite.uv_rect;
        gs.normal_index = sprite.normal_index;
        gs.roughness = sprite.roughness;
        gs.metallic = sprite.metallic;
        gs.emission = sprite.emission;

        uint32_t idx = static_cast<uint32_t>(sprites.size());
        sprites.push_back(gs);

        Bucket bucket;
        if (!sprite.lit)        bucket = Bucket::UnlitTransparent;
        else if (sprite.flat)   bucket = Bucket::LitOpaque;
        else                    bucket = Bucket::LitTransparent;

        glm::vec3 diff = camera_position - pos;
        float dist_sq = glm::dot(diff, diff);
        sort_entries.push_back({bucket, sprite.sort_layer, sprite.sort_order,
                                pos.y, dist_sq, idx, sprite.flat});
    }

    // Opaque sprites after the transparent ones, which sort by layer and
    // order together, unlit and lit billboards alike, so they blend in the
    // order the layers ask. Within one layer and order, flat sprites come
    // first (higher y behind), then camera-facing ones, farthest first: flat
    // sprites are the plane the others stand on, and height and distance do
    // not compare with each other.
    std::sort(sort_entries.begin(), sort_entries.end(),
              [](const SortEntry& a, const SortEntry& b) {
                  const bool a_opaque = a.bucket == Bucket::LitOpaque;
                  const bool b_opaque = b.bucket == Bucket::LitOpaque;
                  if (a_opaque != b_opaque) return b_opaque;
                  if (a.sort_layer != b.sort_layer) return a.sort_layer < b.sort_layer;
                  if (a.sort_order != b.sort_order) return a.sort_order < b.sort_order;
                  if (a.flat != b.flat) return a.flat;
                  if (a.flat) return a.y_position > b.y_position;
                  return a.distance > b.distance;
              });

    uint32_t count = std::min(static_cast<uint32_t>(sort_entries.size()), MAX_SPRITES);

    auto& runs = transparent_runs_[frame_index];
    auto& opaque = opaque_range_[frame_index];
    runs.clear();
    opaque = {Bucket::LitOpaque, 0, 0};

    if (count > 0) {
        auto* dst = static_cast<GpuSprite*>(sprite_ssbos_[frame_index]->mapped());
        for (uint32_t i = 0; i < count; ++i) {
            dst[i] = sprites[sort_entries[i].index];
        }
    }
    for (uint32_t i = 0; i < count; ++i) {
        const Bucket bucket = sort_entries[i].bucket;
        if (bucket == Bucket::LitOpaque) {
            if (opaque.count == 0) opaque.first = i;
            ++opaque.count;
        } else if (!runs.empty() && runs.back().bucket == bucket) {
            ++runs.back().count;
        } else {
            runs.push_back({bucket, i, 1});
        }
    }
}

void SpriteRenderer::record_runs(const FrameContext& ctx, std::span<const SpriteRun> runs) {
    FJELL_PROFILE_SCOPE_N("sprite_record_runs");
    if (std::ranges::all_of(runs, [](const SpriteRun& run) { return run.count == 0; })) return;

    VkViewport vp{};
    vp.width = static_cast<float>(ctx.target->extent().width);
    vp.height = static_cast<float>(ctx.target->extent().height);
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(ctx.cmd, 0, 1, &vp);

    VkRect2D sc{};
    sc.extent = ctx.target->extent();
    vkCmdSetScissor(ctx.cmd, 0, 1, &sc);

    VkDescriptorSet sets[] = {ctx.global_descriptor_set, ctx.bindless_set, sprite_sets_[ctx.frame_index]};
    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_,
                            0, 3, sets, 0, nullptr);

    VkPipeline bound = VK_NULL_HANDLE;
    for (const SpriteRun& run : runs) {
        if (run.count == 0) continue;
        const VkPipeline wanted = pipelines_[static_cast<size_t>(run.bucket)];
        if (wanted != bound) {
            vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, wanted);
            bound = wanted;
        }
        // Each sprite is 6 vertices. The vertex shader keys off
        // gl_VertexIndex / 6 to look up the sprite in the SSBO; firstVertex
        // shifts that onto the run without reuploading anything.
        vkCmdDraw(ctx.cmd, run.count * 6, 1, run.first * 6, 0);
    }
}

} // namespace fjell
