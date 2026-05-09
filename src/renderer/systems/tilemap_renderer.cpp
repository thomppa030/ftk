#include "renderer/systems/tilemap_renderer.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/resources/shader_utils.hpp"
#include "renderer/resources/texture.hpp"
#include "renderer/components/tilemap_component.hpp"
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

void TilemapRenderer::create(Device& device, const PassResources& resources) {
    device_ = device.handle();
    vk_pipeline_cache_ = resources.pipeline_cache;
    allocator_ = resources.allocator;
    global_set_layout_ = resources.global_set_layout;
    bindless_set_layout_ = resources.material_set_layout;
    color_format_ = resources.color_format;
    depth_format_ = resources.depth_format;
    sample_count_ = resources.sample_count;

    // Tile SSBO descriptor set layout (set 2, binding 0)
    VkDescriptorSetLayoutBinding ssbo_binding{};
    ssbo_binding.binding = 0;
    ssbo_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ssbo_binding.descriptorCount = 1;
    ssbo_binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_ci.bindingCount = 1;
    layout_ci.pBindings = &ssbo_binding;
    if (vkCreateDescriptorSetLayout(device_, &layout_ci, nullptr, &tile_set_layout_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create tilemap set layout");
    }

    // Push constant range for texture_index + z_depth
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(PushConstants);

    VkDescriptorSetLayout sets[] = {global_set_layout_, bindless_set_layout_, tile_set_layout_};
    VkPipelineLayoutCreateInfo pipe_layout_ci{};
    pipe_layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipe_layout_ci.setLayoutCount = 3;
    pipe_layout_ci.pSetLayouts = sets;
    pipe_layout_ci.pushConstantRangeCount = 1;
    pipe_layout_ci.pPushConstantRanges = &push_range;
    if (vkCreatePipelineLayout(device_, &pipe_layout_ci, nullptr, &layout_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create tilemap pipeline layout");
    }

    auto unlit_vert = create_shader_module(device_, read_spirv(base_path::resolve("shaders/tilemap.vert.spv")));
    auto unlit_frag = create_shader_module(device_, read_spirv(base_path::resolve("shaders/tilemap.frag.spv")));
    auto lit_vert   = create_shader_module(device_, read_spirv(base_path::resolve("shaders/tilemap_lit.vert.spv")));
    auto lit_frag   = create_shader_module(device_, read_spirv(base_path::resolve("shaders/tilemap_lit.frag.spv")));

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
                              bool depth_write, bool alpha_blend, bool write_gbuffer,
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

        VkPipelineColorBlendAttachmentState mr_blend{};
        mr_blend.colorWriteMask = write_gbuffer
            ? (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT) : 0;
        mr_blend.blendEnable = VK_FALSE;

        VkPipelineColorBlendAttachmentState normal_blend{};
        normal_blend.colorWriteMask = write_gbuffer
            ? (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT) : 0;
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
            throw std::runtime_error("Failed to create tilemap pipeline");
        }
    };

    build_pipeline(unlit_vert, unlit_frag, /*depth_write=*/false, /*alpha_blend=*/true,  /*write_gbuffer=*/false,
                   pipelines_[static_cast<size_t>(Bucket::UnlitTransparent)]);
    build_pipeline(lit_vert,   lit_frag,   /*depth_write=*/true,  /*alpha_blend=*/false, /*write_gbuffer=*/true,
                   pipelines_[static_cast<size_t>(Bucket::LitOpaque)]);

    // Depth-only pipeline (lit-opaque tilemap → depth prepass).
    {
        auto depth_frag = create_shader_module(device_, read_spirv(base_path::resolve("shaders/tilemap_depth.frag.spv")));

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
            throw std::runtime_error("Failed to create tilemap depth pipeline");
        }
        vkDestroyShaderModule(device_, depth_frag, nullptr);
    }

    vkDestroyShaderModule(device_, unlit_vert, nullptr);
    vkDestroyShaderModule(device_, unlit_frag, nullptr);
    vkDestroyShaderModule(device_, lit_vert, nullptr);
    vkDestroyShaderModule(device_, lit_frag, nullptr);

    // Descriptor pool
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = MAX_FRAMES_IN_FLIGHT;

    VkDescriptorPoolCreateInfo desc_pool_ci{};
    desc_pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    desc_pool_ci.maxSets = MAX_FRAMES_IN_FLIGHT;
    desc_pool_ci.poolSizeCount = 1;
    desc_pool_ci.pPoolSizes = &pool_size;
    if (vkCreateDescriptorPool(device_, &desc_pool_ci, nullptr, &pool_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create tilemap descriptor pool");
    }

    // Double-buffered SSBOs + descriptor sets
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        tile_ssbos_[i] = std::make_unique<Buffer>(
            allocator_, SSBO_SIZE,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VMA_MEMORY_USAGE_AUTO, VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
        tile_ssbos_[i]->map();

        VkDescriptorSetAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool = pool_;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &tile_set_layout_;
        if (vkAllocateDescriptorSets(device_, &alloc, &tile_sets_[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to allocate tilemap descriptor set");
        }

        VkDescriptorBufferInfo buf_info{tile_ssbos_[i]->handle(), 0, SSBO_SIZE};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = tile_sets_[i];
        write.dstBinding = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.descriptorCount = 1;
        write.pBufferInfo = &buf_info;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }

    FJELL_GFX_DEBUG("Tilemap renderer created");
}

void TilemapRenderer::destroy() {
    auto safe_destroy = [&](auto& handle, auto destroy_fn) {
        if (handle != VK_NULL_HANDLE) { destroy_fn(device_, handle, nullptr); handle = VK_NULL_HANDLE; }
    };

    for (auto& ssbo : tile_ssbos_) ssbo.reset();
    safe_destroy(pool_, vkDestroyDescriptorPool);
    for (auto& pipe : pipelines_) safe_destroy(pipe, vkDestroyPipeline);
    safe_destroy(depth_pipeline_, vkDestroyPipeline);
    safe_destroy(layout_, vkDestroyPipelineLayout);
    safe_destroy(tile_set_layout_, vkDestroyDescriptorSetLayout);
}

void TilemapRenderer::sync_textures(Scene& scene, ResourceManager& resources, const Project* project) {
    for (auto& obj : scene.objects()) {
        auto* tm = obj->components.get<TilemapComponent>();
        if (!tm) continue;

        if (!tm->texture_loaded && !tm->tileset_texture.empty()) {
            std::string resolved = base_path::resolve_asset(tm->tileset_texture, project);
            if (std::filesystem::exists(resolved)) {
                auto tex_result = tm->pixel_art
                    ? resources.load_texture_nearest(resolved)
                    : resources.load_texture(resolved);
                if (tex_result) {
                    tm->texture_index = resources.bindless_index(*tex_result);
                    auto* texture = resources.get(*tex_result);
                    if (texture) {
                        tm->tex_width = texture->width();
                        tm->tex_height = texture->height();
                        tm->tileset_columns = tm->tex_width / static_cast<uint32_t>(tm->tile_size.x);
                    }
                    tm->texture_loaded = true;
                    tm->dirty = true;
                }
            }
        }

        if (!tm->normal_loaded && !tm->normal_path.empty()) {
            std::string resolved = base_path::resolve_asset(tm->normal_path, project);
            if (std::filesystem::exists(resolved)) {
                auto nm_result = tm->pixel_art
                    ? resources.load_texture_nearest(resolved)
                    : resources.load_texture(resolved);
                if (nm_result) {
                    tm->normal_index = resources.bindless_index(*nm_result);
                    tm->normal_loaded = true;
                }
            }
        }
    }
}

void TilemapRenderer::update(const RenderSubsystemContext& ctx) {
    if (ctx.scene) {
        update_impl(*ctx.scene, ctx.view_projection, ctx.frame_index);
    }
}

void TilemapRenderer::record_depth(const FrameContext& ctx) {
    FJELL_PROFILE_SCOPE_N("tilemap_record_depth");
    if (tile_buckets_[ctx.frame_index] != Bucket::LitOpaque) return;
    if (tile_counts_[ctx.frame_index] == 0) return;

    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, depth_pipeline_);

    VkViewport vp{};
    vp.width = static_cast<float>(ctx.target->extent().width);
    vp.height = static_cast<float>(ctx.target->extent().height);
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(ctx.cmd, 0, 1, &vp);

    VkRect2D sc{};
    sc.extent = ctx.target->extent();
    vkCmdSetScissor(ctx.cmd, 0, 1, &sc);

    VkDescriptorSet sets[] = {ctx.global_descriptor_set, ctx.bindless_set, tile_sets_[ctx.frame_index]};
    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_,
                            0, 3, sets, 0, nullptr);

    vkCmdPushConstants(ctx.cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                       sizeof(PushConstants), &push_constants_[ctx.frame_index]);

    vkCmdDraw(ctx.cmd, tile_counts_[ctx.frame_index] * 6, 1, 0, 0);
}

void TilemapRenderer::record_opaque(const FrameContext& ctx) {
    if (tile_buckets_[ctx.frame_index] == Bucket::LitOpaque) {
        record_bucket(ctx, Bucket::LitOpaque);
    }
}

void TilemapRenderer::record_transparent(const FrameContext& ctx) {
    if (tile_buckets_[ctx.frame_index] == Bucket::UnlitTransparent) {
        record_bucket(ctx, Bucket::UnlitTransparent);
    }
}

void TilemapRenderer::update_impl(const Scene& scene, const glm::mat4& /*view_projection*/,
                                   uint32_t frame_index) {
    FJELL_PROFILE_SCOPE_N("tilemap_update");

    uint32_t total_tiles = 0;
    PushConstants pc{};

    // For now we just use the first tilemap found
    // TODO: support multiple tilemaps
    for (const auto& obj : scene.objects()) {
        const auto* tm = obj->components.get<TilemapComponent>();
        if (!tm || !tm->texture_loaded) continue;
        if (tm->tiles.empty()) continue;

        const auto* spatial = obj->as_spatial();
        if (!spatial) continue;
        glm::vec3 origin = glm::vec3(spatial->world_matrix_[3]);

        pc.texture_index = tm->texture_index;
        pc.z_depth = origin.z;
        pc.normal_index = tm->normal_index;
        pc.flags = tm->lit ? 1u : 0u;
        pc.roughness = tm->roughness;
        pc.metallic = tm->metallic;
        pc.emission = tm->emission;

        tile_buckets_[frame_index] = tm->lit ? Bucket::LitOpaque : Bucket::UnlitTransparent;

        int cols = tm->map_size.x;
        int rows = tm->map_size.y;

        auto* dst = static_cast<GpuTile*>(tile_ssbos_[frame_index]->mapped());

        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                int idx = r * cols + c;
                if (idx >= static_cast<int>(tm->tiles.size())) break;
                int32_t tile_id = tm->tiles[idx];
                if (tile_id < 0) continue;
                if (total_tiles >= MAX_TILES) break;

                // Tile position in world space
                float x = origin.x + static_cast<float>(c) * tm->world_tile_size;
                float y = origin.y - static_cast<float>(r) * tm->world_tile_size; // row 0 = top

                dst[total_tiles].position = {x, y};
                dst[total_tiles].size = {tm->world_tile_size, tm->world_tile_size};

                // Compute UV from tile_id using tileset grid
                uint32_t tileset_cols = tm->tileset_columns;
                if (tileset_cols == 0) continue;
                uint32_t tc = static_cast<uint32_t>(tile_id) % tileset_cols;
                uint32_t tr = static_cast<uint32_t>(tile_id) / tileset_cols;
                float tw = static_cast<float>(tm->tile_size.x);
                float th = static_cast<float>(tm->tile_size.y);
                float tex_w = static_cast<float>(tm->tex_width);
                float tex_h = static_cast<float>(tm->tex_height);
                float u0 = (static_cast<float>(tc) * tw) / tex_w;
                float v0 = (static_cast<float>(tr) * th) / tex_h;
                float u1 = u0 + tw / tex_w;
                float v1 = v0 + th / tex_h;
                dst[total_tiles].uv_rect = {u0, v0, u1, v1};

                total_tiles++;
            }
        }
        break; // only first tilemap for now
    }

    tile_counts_[frame_index] = total_tiles;
    push_constants_[frame_index] = pc;
}

void TilemapRenderer::record_bucket(const FrameContext& ctx, Bucket bucket) {
    FJELL_PROFILE_SCOPE_N("tilemap_record_bucket");
    if (tile_counts_[ctx.frame_index] == 0) return;

    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      pipelines_[static_cast<size_t>(bucket)]);

    VkViewport vp{};
    vp.width = static_cast<float>(ctx.target->extent().width);
    vp.height = static_cast<float>(ctx.target->extent().height);
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(ctx.cmd, 0, 1, &vp);

    VkRect2D sc{};
    sc.extent = ctx.target->extent();
    vkCmdSetScissor(ctx.cmd, 0, 1, &sc);

    VkDescriptorSet sets[] = {ctx.global_descriptor_set, ctx.bindless_set, tile_sets_[ctx.frame_index]};
    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_,
                            0, 3, sets, 0, nullptr);

    vkCmdPushConstants(ctx.cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                       sizeof(PushConstants), &push_constants_[ctx.frame_index]);

    vkCmdDraw(ctx.cmd, tile_counts_[ctx.frame_index] * 6, 1, 0, 0);
}

} // namespace fjell
