#include "renderer/systems/sprite_renderer.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/resources/texture.hpp"
#include "renderer/resources/shader_utils.hpp"
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

    // Shaders
    auto vert_mod = create_shader_module(device_, read_spirv(base_path::resolve("shaders/sprite.vert.spv")));
    auto frag_mod = create_shader_module(device_, read_spirv(base_path::resolve("shaders/sprite.frag.spv")));

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert_mod;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag_mod;
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

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_FALSE;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    // Alpha blending
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendAttachmentState mr_blend{};
    mr_blend.colorWriteMask = 0; // Sprites don't write MR
    mr_blend.blendEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState blend_attachments[] = {blend, mr_blend};

    VkPipelineColorBlendStateCreateInfo color_blending{};
    color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blending.attachmentCount = 2;
    color_blending.pAttachments = blend_attachments;

    VkFormat color_formats[] = {color_format_, VK_FORMAT_R8G8_UNORM};

    VkPipelineRenderingCreateInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_info.colorAttachmentCount = 2;
    rendering_info.pColorAttachmentFormats = color_formats;
    rendering_info.depthAttachmentFormat = depth_format_;

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

    if (vkCreateGraphicsPipelines(device_, vk_pipeline_cache_, 1, &ci, nullptr, &pipeline_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create sprite pipeline");
    }

    vkDestroyShaderModule(device_, vert_mod, nullptr);
    vkDestroyShaderModule(device_, frag_mod, nullptr);

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
    safe_destroy(pipeline_, vkDestroyPipeline);
    safe_destroy(layout_, vkDestroyPipelineLayout);
    safe_destroy(sprite_set_layout_, vkDestroyDescriptorSetLayout);
}

void SpriteRenderer::sync_textures(Scene& scene, ResourceManager& resources, const Project* project) {
    FJELL_PROFILE_SCOPE_N("sprite_sync_textures");
    for (auto& obj : scene.objects()) {
        auto* sprite_ptr = obj->components.get<SpriteComponent>();
        if (!sprite_ptr || sprite_ptr->texture_loaded) continue;

        auto& sprite = *sprite_ptr;
        if (sprite.texture_path.empty()) continue;

        std::string resolved = base_path::resolve_asset(sprite.texture_path, project);
        if (!std::filesystem::exists(resolved)) continue;

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

void SpriteRenderer::update(const Scene& scene, const glm::mat4& view_projection,
                            const glm::vec3& camera_position, uint32_t frame_index,
                            bool playing) {
    FJELL_PROFILE_SCOPE_N("sprite_update");
    Frustum frustum = Frustum::from_vp_matrix(view_projection);

    // Gather visible sprites (reuse persistent scratch vectors)
    auto& sprites = sprites_scratch_;
    auto& sort_entries = sort_scratch_;
    sprites.clear();
    sort_entries.clear();

    for (const auto& obj : scene.objects()) {
        const auto* sp = obj->components.get<SpriteComponent>();
        if (!sp || !sp->texture_loaded) continue;
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
        gs.size = half_size;
        gs.texture_index = sprite.texture_index;
        gs.opacity = sprite.opacity;
        gs.tint = sprite.tint;
        gs.uv_rect = sprite.uv_rect;

        uint32_t idx = static_cast<uint32_t>(sprites.size());
        sprites.push_back(gs);

        glm::vec3 diff = camera_position - pos;
        float dist_sq = glm::dot(diff, diff);
        sort_entries.push_back({sprite.sort_layer, sprite.sort_order,
                                pos.y, dist_sq, idx, sprite.flat});
    }

    // Sort: by layer first, then sub-order, then Y position (flat) or distance (billboard)
    std::sort(sort_entries.begin(), sort_entries.end(),
              [](const SortEntry& a, const SortEntry& b) {
                  if (a.sort_layer != b.sort_layer) return a.sort_layer < b.sort_layer;
                  if (a.sort_order != b.sort_order) return a.sort_order < b.sort_order;
                  // Flat sprites: lower Y draws first (behind) for top-down
                  if (a.flat && b.flat) return a.y_position > b.y_position;
                  // Billboard sprites: back-to-front by distance
                  return a.distance > b.distance;
              });

    // Write sorted sprites to SSBO
    uint32_t count = std::min(static_cast<uint32_t>(sort_entries.size()), MAX_SPRITES);
    sprite_counts_[frame_index] = count;

    if (count > 0) {
        auto* dst = static_cast<GpuSprite*>(sprite_ssbos_[frame_index]->mapped());
        for (uint32_t i = 0; i < count; ++i) {
            dst[i] = sprites[sort_entries[i].index];
        }
    }
}

void SpriteRenderer::record_render(const FrameContext& ctx) {
    FJELL_PROFILE_SCOPE_N("sprite_record_render");
    if (sprite_counts_[ctx.frame_index] == 0) return;

    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

    VkViewport vp{};
    vp.width = static_cast<float>(ctx.target->extent().width);
    vp.height = static_cast<float>(ctx.target->extent().height);
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(ctx.cmd, 0, 1, &vp);

    VkRect2D sc{};
    sc.extent = ctx.target->extent();
    vkCmdSetScissor(ctx.cmd, 0, 1, &sc);

    // Bind set 0 = global, set 1 = bindless, set 2 = sprite SSBO
    VkDescriptorSet sets[] = {ctx.global_descriptor_set, ctx.bindless_set, sprite_sets_[ctx.frame_index]};
    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_,
                            0, 3, sets, 0, nullptr);

    vkCmdDraw(ctx.cmd, sprite_counts_[ctx.frame_index] * 6, 1, 0, 0);
}

} // namespace fjell
