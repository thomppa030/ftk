#include "renderer/pass/local_shadow_pass.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/gpu/vma_image.hpp"
#include "renderer/gpu_context.hpp"
#include "renderer/indirect_batch.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/renderer_constants.hpp"
#include "renderer/resources/mega_buffer.hpp"
#include "renderer/resources/model.hpp"
#include "renderer/resources/shader_utils.hpp"
#include "renderer/resources/skeletal_model.hpp"
#include "renderer/resources/vertex.hpp"
#include "renderer/render_queue.hpp"
#include "scene/light_node.hpp"
#include "scene/scene.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#ifdef FJELL_ENABLE_TRACY
#include <tracy/TracyVulkan.hpp>
#endif

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace fjell {

// ── Atlas image ─────────────────────────────────────────────────────────

void LocalShadowPass::create_atlas() {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = DEPTH_FORMAT;
    info.extent = {ATLAS_SIZE, ATLAS_SIZE, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (create_vma_image(allocator_, &info, &atlas_image_, &atlas_memory_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create local shadow atlas");
    }

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = atlas_image_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = DEPTH_FORMAT;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};

    if (vkCreateImageView(device_, &view_info, nullptr, &atlas_view_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create local shadow atlas view");
    }

    // Comparison sampler (same as cascade shadows)
    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    sampler_info.compareEnable = VK_TRUE;
    sampler_info.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    if (vkCreateSampler(device_, &sampler_info, nullptr, &sampler_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create local shadow sampler");
    }

    FJELL_GFX_DEBUG("Local shadow atlas created: {}x{}", ATLAS_SIZE, ATLAS_SIZE);
}

void LocalShadowPass::destroy_atlas() {
    if (sampler_ != VK_NULL_HANDLE) { vkDestroySampler(device_, sampler_, nullptr); sampler_ = VK_NULL_HANDLE; }
    if (atlas_view_ != VK_NULL_HANDLE) { vkDestroyImageView(device_, atlas_view_, nullptr); atlas_view_ = VK_NULL_HANDLE; }
    destroy_vma_image(allocator_, atlas_image_, atlas_memory_);
    atlas_image_ = VK_NULL_HANDLE;
    atlas_memory_ = VK_NULL_HANDLE;
}

// ── Pipeline ────────────────────────────────────────────────────────────

Result<> LocalShadowPass::create(Device& device, const PassResources& resources) {
    device_ = device.handle();
    vk_pipeline_cache_ = resources.pipeline_cache;
    allocator_ = resources.allocator;
    draw_data_layout_ = resources.draw_data_layout;
    global_set_layout_ = resources.global_set_layout;
    material_set_layout_ = resources.material_set_layout;

    create_atlas();

    // Reuse the same shadow vertex + depth-only fragment shaders as cascade shadows
    auto vert_code = read_spirv(base_path::resolve("shaders/shadow.vert.spv"));
    auto frag_code = read_spirv(base_path::resolve("shaders/depth_only.frag.spv"));
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
    rasterizer.depthClampEnable = VK_TRUE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_FRONT_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_TRUE;
    rasterizer.depthBiasConstantFactor = 1.25f;
    rasterizer.depthBiasSlopeFactor = 1.75f;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendStateCreateInfo color_blending{};
    color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blending.attachmentCount = 0;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_TRUE;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(glm::mat4);

    std::array set_layouts = {global_set_layout_, material_set_layout_, draw_data_layout_};

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = static_cast<uint32_t>(set_layouts.size());
    layout_info.pSetLayouts = set_layouts.data();
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;

    if (vkCreatePipelineLayout(device_, &layout_info, nullptr, &layout_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, vert_module, nullptr);
        vkDestroyShaderModule(device_, frag_module, nullptr);
        return make_error("Failed to create local shadow pipeline layout");
    }

    VkPipelineRenderingCreateInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_info.colorAttachmentCount = 0;
    rendering_info.depthAttachmentFormat = DEPTH_FORMAT;

    VkGraphicsPipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.pNext = &rendering_info;
    ci.stageCount = static_cast<uint32_t>(stages.size());
    ci.pStages = stages.data();
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
        vkDestroyShaderModule(device_, vert_module, nullptr);
        vkDestroyShaderModule(device_, frag_module, nullptr);
        return make_error("Failed to create local shadow pipeline");
    }

    // Skinned variant
    auto skinned_vert = read_spirv(base_path::resolve("shaders/skinned_shadow.vert.spv"));
    auto skinned_mod = create_shader_module(device_, skinned_vert);
    stages[0].module = skinned_mod;

    auto skinned_bind = SkinnedVertex::get_binding_description();
    auto skinned_attr = SkinnedVertex::get_attribute_descriptions();
    vertex_input.pVertexBindingDescriptions = &skinned_bind;
    vertex_input.vertexAttributeDescriptionCount = static_cast<uint32_t>(skinned_attr.size());
    vertex_input.pVertexAttributeDescriptions = skinned_attr.data();
    rasterizer.cullMode = VK_CULL_MODE_NONE;

    ci.pStages = stages.data();
    ci.pVertexInputState = &vertex_input;

    if (vkCreateGraphicsPipelines(device_, vk_pipeline_cache_, 1, &ci, nullptr, &skinned_pipeline_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, skinned_mod, nullptr);
        vkDestroyShaderModule(device_, vert_module, nullptr);
        vkDestroyShaderModule(device_, frag_module, nullptr);
        return make_error("Failed to create skinned local shadow pipeline");
    }

    vkDestroyShaderModule(device_, skinned_mod, nullptr);
    vkDestroyShaderModule(device_, vert_module, nullptr);
    vkDestroyShaderModule(device_, frag_module, nullptr);

    FJELL_GFX_DEBUG("Local shadow pass created");
    return {};
}

void LocalShadowPass::post_create(GpuContext& gpu, PassRegistry& /*passes*/) {
    auto& gb = gpu.global_bindings();
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        gb.write_image(i, "local_shadow_atlas",
            atlas_view_, sampler_,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    }
    gb.flush_writes();
}

void LocalShadowPass::destroy() {
    if (pipeline_ != VK_NULL_HANDLE) { vkDestroyPipeline(device_, pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
    if (skinned_pipeline_ != VK_NULL_HANDLE) { vkDestroyPipeline(device_, skinned_pipeline_, nullptr); skinned_pipeline_ = VK_NULL_HANDLE; }
    if (layout_ != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device_, layout_, nullptr); layout_ = VK_NULL_HANDLE; }
    destroy_atlas();
}

// ── Per-frame tile assignment ──────────────────────────────────────────

void LocalShadowPass::assign_spot_tiles(const FrameContext& ctx) {
    spot_tiles_.clear();
    if (!active_scene_) return;

    uint32_t tile_x = 0;
    uint32_t tile_y = 0;
    uint32_t tile_size = LOCAL_SHADOW_TILE_SIZE;
    uint32_t tiles_per_row = ATLAS_SIZE / tile_size;

    // Collect shadowed spot lights, sorted by distance to camera
    struct SpotCandidate {
        const SpotLightNode* node;
        uint32_t light_index;
        float distance_sq;
    };
    std::vector<SpotCandidate> candidates;

    uint32_t spot_index = 0;
    for (const auto& obj : active_scene_->objects()) {
        auto* sl = dynamic_cast<const SpotLightNode*>(obj.get());
        if (!sl) continue;
        uint32_t this_index = spot_index++;
        if (!sl->cast_shadows) continue;

        glm::vec3 pos = glm::vec3(sl->world_matrix_[3]);
        glm::vec3 diff = pos - ctx.camera_position;
        candidates.push_back({sl, this_index, glm::dot(diff, diff)});
    }

    // Sort by distance, take closest N
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) { return a.distance_sq < b.distance_sq; });

    uint32_t count = std::min(static_cast<uint32_t>(candidates.size()),
                              MAX_SHADOWED_SPOT_LIGHTS);

    for (uint32_t i = 0; i < count; ++i) {
        const auto& c = candidates[i];
        const auto* sl = c.node;

        glm::vec3 pos = glm::vec3(sl->world_matrix_[3]);
        glm::vec3 dir = glm::normalize(glm::mat3(sl->world_matrix_) * sl->direction);

        // Perspective projection matching the spot cone
        float fov = glm::radians(sl->outer_angle) * 2.0f;
        fov = std::min(fov, glm::radians(170.0f)); // clamp to avoid degenerate projection
        glm::mat4 proj = glm::perspectiveZO(fov, 1.0f, 0.1f, sl->radius);

        // View matrix: look from light position along direction
        glm::vec3 up = (std::abs(dir.y) > 0.99f) ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
        glm::mat4 view = glm::lookAt(pos, pos + dir, up);

        ShadowTile tile{};
        tile.x = tile_x * tile_size;
        tile.y = tile_y * tile_size;
        tile.size = tile_size;
        tile.light_vp = proj * view;
        tile.light_index = c.light_index;
        tile.is_spot = true;
        spot_tiles_.push_back(tile);

        // Advance tile position in atlas grid
        tile_x++;
        if (tile_x >= tiles_per_row) {
            tile_x = 0;
            tile_y++;
        }
    }
}

// ── Point light tile assignment ───────────────────────────────────────

void LocalShadowPass::assign_point_tiles(const FrameContext& ctx) {
    point_blocks_.clear();
    if (!active_scene_) return;

    uint32_t tile_size = LOCAL_SHADOW_TILE_SIZE;

    // Point light blocks go after spot tiles in the atlas.
    // Each point light uses a 2x3 block of tiles (6 faces).
    // Start from the row after the last spot tile row.
    uint32_t spot_rows = !spot_tiles_.empty()
        ? (spot_tiles_.back().y / tile_size) + 1
        : 0;

    // Point blocks are 2 tiles wide, 3 tiles tall
    uint32_t block_w = 2 * tile_size;
    uint32_t block_h = 3 * tile_size;

    uint32_t block_x = 0;
    uint32_t block_y = spot_rows * tile_size;

    // Collect shadowed point lights, sorted by distance to camera
    struct PointCandidate {
        const PointLightNode* node;
        uint32_t light_index;
        float distance_sq;
    };
    std::vector<PointCandidate> candidates;

    uint32_t point_index = 0;
    for (const auto& obj : active_scene_->objects()) {
        auto* pl = dynamic_cast<const PointLightNode*>(obj.get());
        if (!pl) continue;
        uint32_t this_index = point_index++;
        if (!pl->cast_shadows) continue;

        glm::vec3 pos = glm::vec3(pl->world_matrix_[3]);
        glm::vec3 diff = pos - ctx.camera_position;
        candidates.push_back({pl, this_index, glm::dot(diff, diff)});
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) { return a.distance_sq < b.distance_sq; });

    uint32_t count = std::min(static_cast<uint32_t>(candidates.size()),
                              MAX_SHADOWED_POINT_LIGHTS);

    // Standard cubemap face directions and up vectors
    static const glm::vec3 face_dirs[6] = {
        { 1,  0,  0}, // +X
        {-1,  0,  0}, // -X
        { 0,  1,  0}, // +Y
        { 0, -1,  0}, // -Y
        { 0,  0,  1}, // +Z
        { 0,  0, -1}, // -Z
    };
    static const glm::vec3 face_ups[6] = {
        { 0, -1,  0}, // +X
        { 0, -1,  0}, // -X
        { 0,  0,  1}, // +Y
        { 0,  0, -1}, // -Y
        { 0, -1,  0}, // +Z
        { 0, -1,  0}, // -Z
    };

    constexpr float near = 0.1f;

    for (uint32_t i = 0; i < count; ++i) {
        const auto& c = candidates[i];
        glm::vec3 pos = glm::vec3(c.node->world_matrix_[3]);
        float radius = c.node->radius;

        // Check atlas overflow
        if (block_y + block_h > ATLAS_SIZE) break;

        glm::mat4 proj = glm::perspectiveZO(glm::radians(90.0f), 1.0f, near, radius);

        PointShadowBlock block{};
        block.x = block_x;
        block.y = block_y;
        block.tile_size = tile_size;
        block.light_index = c.light_index;
        block.near_plane = near;
        block.far_plane = radius;

        for (int f = 0; f < 6; ++f) {
            block.face_vp[f] = proj * glm::lookAt(pos, pos + face_dirs[f], face_ups[f]);
        }

        point_blocks_.push_back(block);

        // Advance block position
        block_x += block_w;
        if (block_x + block_w > ATLAS_SIZE) {
            block_x = 0;
            block_y += block_h;
        }
    }
}

void LocalShadowPass::prepare_frame(const FrameContext& ctx, GpuContext& /*gpu*/) {
    active_scene_ = const_cast<Scene*>(ctx.scene);
    assign_spot_tiles(ctx);
    assign_point_tiles(ctx);
}

// ── Setup / build ──────────────────────────────────────────────────────

PassSetup LocalShadowPass::setup() const {
    return {
        .name = "local_shadows",
        .phase = RenderPhase::Environment,
        .sub_order = 110, // after cascade shadows (100)
        .reads = {},
        .writes = {},
    };
}

void LocalShadowPass::declare(PassBuilder& /*builder*/, const DeclareContext& /*ctx*/) {
    // Shadow atlas is pass-private; layout transitions happen inline in
    // record(). Graph sees an empty-access pass.
}

void LocalShadowPass::build(FrameGraph& graph, ResourceRegistry& /*registry*/,
                             const FrameContext& ctx) {
    PassBuilder builder;
    graph.submit_declared_pass("local_shadows", builder,
        [this, ctx](VkCommandBuffer cb) {
            FrameContext local = ctx;
            local.cmd = cb;
            record(local);
        });
}

// ── Recording ──────────────────────────────────────────────────────────

void LocalShadowPass::record(const FrameContext& ctx) {
    bool has_work = !spot_tiles_.empty() || !point_blocks_.empty();
    if (!has_work) {
        // No shadow-casting lights, but the atlas descriptor is still bound.
        // Transition from UNDEFINED to READ_ONLY so sampling returns 1.0 (white = no shadow).
        VkImageMemoryBarrier2 init_barrier{};
        init_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        init_barrier.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        init_barrier.srcAccessMask = 0;
        init_barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        init_barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        init_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        init_barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        init_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        init_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        init_barrier.image = atlas_image_;
        init_barrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};

        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &init_barrier;
        vkCmdPipelineBarrier2(ctx.cmd, &dep);
        return;
    }
    if (!ctx.mega_buffer || !ctx.indirect_batch) return;

#ifdef FJELL_ENABLE_TRACY
    TracyVkZone(static_cast<tracy::VkCtx*>(ctx.tracy_ctx), ctx.cmd, "Local Shadows");
#endif

    auto cmd = ctx.cmd;

    // Transition entire atlas to depth attachment
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = atlas_image_;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};

    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);

    // Clear the entire atlas first
    VkRenderingAttachmentInfo clear_att{};
    clear_att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    clear_att.imageView = atlas_view_;
    clear_att.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    clear_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    clear_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    clear_att.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo clear_ri{};
    clear_ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    clear_ri.renderArea = {{0, 0}, {ATLAS_SIZE, ATLAS_SIZE}};
    clear_ri.layerCount = 1;
    clear_ri.pDepthAttachment = &clear_att;
    vkCmdBeginRendering(cmd, &clear_ri);
    vkCmdEndRendering(cmd);

    // Render each spot light tile
    std::array descriptor_sets = {
        ctx.global_descriptor_set,
        ctx.bindless_set,
        ctx.draw_data_set};

    for (const auto& tile : spot_tiles_) {
        VkRenderingAttachmentInfo depth_att{};
        depth_att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth_att.imageView = atlas_view_;
        depth_att.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth_att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; // preserve cleared/previous tiles
        depth_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingInfo ri{};
        ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea.offset = {static_cast<int32_t>(tile.x), static_cast<int32_t>(tile.y)};
        ri.renderArea.extent = {tile.size, tile.size};
        ri.layerCount = 1;
        ri.pDepthAttachment = &depth_att;

        vkCmdBeginRendering(cmd, &ri);

        VkViewport vp{};
        vp.x = static_cast<float>(tile.x);
        vp.y = static_cast<float>(tile.y);
        vp.width = static_cast<float>(tile.size);
        vp.height = static_cast<float>(tile.size);
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &vp);

        VkRect2D sc{};
        sc.offset = {static_cast<int32_t>(tile.x), static_cast<int32_t>(tile.y)};
        sc.extent = {tile.size, tile.size};
        vkCmdSetScissor(cmd, 0, 1, &sc);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_,
                                0, static_cast<uint32_t>(descriptor_sets.size()),
                                descriptor_sets.data(), 0, nullptr);

        // Push light VP matrix
        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(glm::mat4), &tile.light_vp);

        // Draw all opaque geometry using indirect
        ctx.mega_buffer->bind(cmd);
        const auto& ranges = ctx.indirect_batch->pipeline_ranges(ctx.frame_index);
        for (const auto& range : ranges) {
            if (range.skinned) continue; // skip skinned in indirect path
            VkDeviceSize offset = range.first_draw * sizeof(VkDrawIndexedIndirectCommand);
            vkCmdDrawIndexedIndirect(cmd,
                ctx.indirect_batch->indirect_buffer(ctx.frame_index),
                offset, range.draw_count,
                sizeof(VkDrawIndexedIndirectCommand));
        }

        // Draw skinned meshes
        const auto& skinned_indices = ctx.indirect_batch->skinned_draw_indices(ctx.frame_index);
        if (!skinned_indices.empty() && ctx.queue) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skinned_pipeline_);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_,
                                    0, static_cast<uint32_t>(descriptor_sets.size()),
                                    descriptor_sets.data(), 0, nullptr);
            vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT,
                               0, sizeof(glm::mat4), &tile.light_vp);

            uint32_t skinned_cmd_idx = 0;
            for (const auto& dc : ctx.queue->commands()) {
                if (!dc.skinned || !dc.skeletal_model) continue;
                if (skinned_cmd_idx >= skinned_indices.size()) break;
                dc.skeletal_model->bind(cmd);
                uint32_t draw_data_idx = skinned_indices[skinned_cmd_idx];
                vkCmdDrawIndexed(cmd, dc.skeletal_model->index_count(), 1, 0, 0, draw_data_idx);
                skinned_cmd_idx++;
            }
        }

        vkCmdEndRendering(cmd);
    }

    // Render point light shadow faces (6 tiles per light in 2x3 layout)
    for (const auto& block : point_blocks_) {
        uint32_t ts = block.tile_size;
        for (int face = 0; face < 6; ++face) {
            uint32_t face_x = block.x + (face % 2) * ts;
            uint32_t face_y = block.y + (face / 2) * ts;

            VkRenderingAttachmentInfo depth_att{};
            depth_att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depth_att.imageView = atlas_view_;
            depth_att.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depth_att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            depth_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

            VkRenderingInfo ri{};
            ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            ri.renderArea.offset = {static_cast<int32_t>(face_x), static_cast<int32_t>(face_y)};
            ri.renderArea.extent = {ts, ts};
            ri.layerCount = 1;
            ri.pDepthAttachment = &depth_att;

            vkCmdBeginRendering(cmd, &ri);

            VkViewport vp{};
            vp.x = static_cast<float>(face_x);
            vp.y = static_cast<float>(face_y);
            vp.width = static_cast<float>(ts);
            vp.height = static_cast<float>(ts);
            vp.minDepth = 0.0f;
            vp.maxDepth = 1.0f;
            vkCmdSetViewport(cmd, 0, 1, &vp);

            VkRect2D sc{};
            sc.offset = {static_cast<int32_t>(face_x), static_cast<int32_t>(face_y)};
            sc.extent = {ts, ts};
            vkCmdSetScissor(cmd, 0, 1, &sc);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_,
                                    0, static_cast<uint32_t>(descriptor_sets.size()),
                                    descriptor_sets.data(), 0, nullptr);

            vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT,
                               0, sizeof(glm::mat4), &block.face_vp[face]);

            ctx.mega_buffer->bind(cmd);
            const auto& ranges = ctx.indirect_batch->pipeline_ranges(ctx.frame_index);
            for (const auto& range : ranges) {
                if (range.skinned) continue;
                VkDeviceSize offset = range.first_draw * sizeof(VkDrawIndexedIndirectCommand);
                vkCmdDrawIndexedIndirect(cmd,
                    ctx.indirect_batch->indirect_buffer(ctx.frame_index),
                    offset, range.draw_count,
                    sizeof(VkDrawIndexedIndirectCommand));
            }

            // Draw skinned meshes
            const auto& skinned_indices = ctx.indirect_batch->skinned_draw_indices(ctx.frame_index);
            if (!skinned_indices.empty() && ctx.queue) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skinned_pipeline_);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_,
                                        0, static_cast<uint32_t>(descriptor_sets.size()),
                                        descriptor_sets.data(), 0, nullptr);
                vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT,
                                   0, sizeof(glm::mat4), &block.face_vp[face]);

                uint32_t skinned_cmd_idx = 0;
                for (const auto& dc : ctx.queue->commands()) {
                    if (!dc.skinned || !dc.skeletal_model) continue;
                    if (skinned_cmd_idx >= skinned_indices.size()) break;
                    dc.skeletal_model->bind(cmd);
                    uint32_t draw_data_idx = skinned_indices[skinned_cmd_idx];
                    vkCmdDrawIndexed(cmd, dc.skeletal_model->index_count(), 1, 0, 0, draw_data_idx);
                    skinned_cmd_idx++;
                }
            }

            vkCmdEndRendering(cmd);
        }
    }

    // Transition atlas to shader read for fragment sampling
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace fjell
