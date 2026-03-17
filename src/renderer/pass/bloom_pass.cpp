#include "renderer/pass/bloom_pass.hpp"
#include "renderer/buffer.hpp"
#include "renderer/device.hpp"
#include "renderer/offscreen_pass.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#include <imgui_impl_vulkan.h>

#include <array>
#include <fstream>
#include <stdexcept>

namespace fjell {

static std::vector<char> read_file(const std::string& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open shader file: " + path);
    }
    auto size = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(size);
    file.seekg(0);
    file.read(buffer.data(), static_cast<std::streamsize>(size));
    return buffer;
}

static VkShaderModule create_module(VkDevice device, const std::vector<char>& code) {
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module;
    if (vkCreateShaderModule(device, &ci, nullptr, &module) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shader module");
    }
    return module;
}

static VkPipeline create_fullscreen_pipeline(
        VkDevice device, VkPipelineLayout layout,
        VkShaderModule vert, VkShaderModule frag, VkFormat color_format) {
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
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
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blend.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo color_blending{};
    color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blending.attachmentCount = 1;
    color_blending.pAttachments = &blend;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

    VkPipelineRenderingCreateInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_info.colorAttachmentCount = 1;
    rendering_info.pColorAttachmentFormats = &color_format;

    VkGraphicsPipelineCreateInfo pipeline_ci{};
    pipeline_ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_ci.pNext = &rendering_info;
    pipeline_ci.stageCount = 2;
    pipeline_ci.pStages = stages.data();
    pipeline_ci.pVertexInputState = &vertex_input;
    pipeline_ci.pInputAssemblyState = &input_assembly;
    pipeline_ci.pViewportState = &viewport_state;
    pipeline_ci.pRasterizationState = &rasterizer;
    pipeline_ci.pMultisampleState = &multisampling;
    pipeline_ci.pDepthStencilState = &depth_stencil;
    pipeline_ci.pColorBlendState = &color_blending;
    pipeline_ci.pDynamicState = &dynamic_state;
    pipeline_ci.layout = layout;

    VkPipeline pipeline;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_ci,
                                   nullptr, &pipeline) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create bloom pipeline");
    }
    return pipeline;
}

// ── Create / Destroy ────────────────────────────────────────────────────

void BloomPass::create(Device& device, const PassResources& /*resources*/) {
    device_ = device.handle();
    physical_device_ = device.physical_device();

    // Sampler (linear, clamp)
    VkSamplerCreateInfo sampler_ci{};
    sampler_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_ci.magFilter = VK_FILTER_LINEAR;
    sampler_ci.minFilter = VK_FILTER_LINEAR;
    sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device_, &sampler_ci, nullptr, &sampler_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create bloom sampler");
    }

    // Descriptor set layouts
    // Single texture layout (for down/upsample)
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_ci.bindingCount = 1;
    layout_ci.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(device_, &layout_ci, nullptr, &single_tex_layout_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create bloom descriptor layout");
    }

    // Composite layout (scene + bloom)
    std::array<VkDescriptorSetLayoutBinding, 2> comp_bindings{};
    comp_bindings[0].binding = 0;
    comp_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    comp_bindings[0].descriptorCount = 1;
    comp_bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    comp_bindings[1].binding = 1;
    comp_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    comp_bindings[1].descriptorCount = 1;
    comp_bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    layout_ci.bindingCount = 2;
    layout_ci.pBindings = comp_bindings.data();
    if (vkCreateDescriptorSetLayout(device_, &layout_ci, nullptr, &composite_layout_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create bloom composite descriptor layout");
    }

    // Descriptor pool: MIP_COUNT shared + FRAMES_IN_FLIGHT source + FRAMES_IN_FLIGHT composite
    uint32_t total_sets = MIP_COUNT + FRAMES_IN_FLIGHT * 2;
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = MIP_COUNT + FRAMES_IN_FLIGHT + FRAMES_IN_FLIGHT * 2;

    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_ci.maxSets = total_sets;
    pool_ci.poolSizeCount = 1;
    pool_ci.pPoolSizes = &pool_size;
    if (vkCreateDescriptorPool(device_, &pool_ci, nullptr, &pool_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create bloom descriptor pool");
    }

    // Allocate mip chain descriptor sets (shared, updated on resize only)
    for (uint32_t i = 0; i < MIP_COUNT; ++i) {
        VkDescriptorSetAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool = pool_;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &single_tex_layout_;
        if (vkAllocateDescriptorSets(device_, &alloc, &mip_sets_[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to allocate bloom mip descriptor set");
        }
    }
    // Per-frame source + composite sets
    for (int f = 0; f < FRAMES_IN_FLIGHT; ++f) {
        VkDescriptorSetAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool = pool_;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &single_tex_layout_;
        if (vkAllocateDescriptorSets(device_, &alloc, &source_sets_[f]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to allocate bloom source descriptor set");
        }

        alloc.pSetLayouts = &composite_layout_;
        if (vkAllocateDescriptorSets(device_, &alloc, &composite_sets_[f]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to allocate bloom composite descriptor set");
        }
    }

    // Pipeline layouts
    VkPushConstantRange down_push{};
    down_push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    down_push.size = 4 * sizeof(float); // src_resolution, threshold, is_first

    VkPipelineLayoutCreateInfo pipe_layout_ci{};
    pipe_layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipe_layout_ci.setLayoutCount = 1;
    pipe_layout_ci.pSetLayouts = &single_tex_layout_;
    pipe_layout_ci.pushConstantRangeCount = 1;
    pipe_layout_ci.pPushConstantRanges = &down_push;
    if (vkCreatePipelineLayout(device_, &pipe_layout_ci, nullptr, &downsample_layout_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create bloom downsample layout");
    }

    VkPushConstantRange up_push{};
    up_push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    up_push.size = 4 * sizeof(float); // src_resolution, blend_factor, padding

    pipe_layout_ci.pPushConstantRanges = &up_push;
    if (vkCreatePipelineLayout(device_, &pipe_layout_ci, nullptr, &upsample_layout_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create bloom upsample layout");
    }

    VkPushConstantRange comp_push{};
    comp_push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    comp_push.size = 4 * sizeof(float); // intensity + padding

    pipe_layout_ci.pSetLayouts = &composite_layout_;
    pipe_layout_ci.pPushConstantRanges = &comp_push;
    if (vkCreatePipelineLayout(device_, &pipe_layout_ci, nullptr, &composite_layout_pipeline_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create bloom composite layout");
    }

    // Shaders
    auto vert_code = read_file(base_path::resolve("shaders/depth_viz.vert.spv")); // reuse fullscreen triangle
    auto down_code = read_file(base_path::resolve("shaders/bloom_downsample.frag.spv"));
    auto up_code   = read_file(base_path::resolve("shaders/bloom_upsample.frag.spv"));
    auto comp_code = read_file(base_path::resolve("shaders/bloom_composite.frag.spv"));

    auto vert_mod = create_module(device_, vert_code);
    auto down_mod = create_module(device_, down_code);
    auto up_mod   = create_module(device_, up_code);
    auto comp_mod = create_module(device_, comp_code);

    VkFormat fmt = VK_FORMAT_R16G16B16A16_SFLOAT;
    downsample_pipeline_ = create_fullscreen_pipeline(device_, downsample_layout_, vert_mod, down_mod, fmt);
    upsample_pipeline_   = create_fullscreen_pipeline(device_, upsample_layout_, vert_mod, up_mod, fmt);
    composite_pipeline_  = create_fullscreen_pipeline(device_, composite_layout_pipeline_, vert_mod, comp_mod, fmt);

    vkDestroyShaderModule(device_, comp_mod, nullptr);
    vkDestroyShaderModule(device_, up_mod, nullptr);
    vkDestroyShaderModule(device_, down_mod, nullptr);
    vkDestroyShaderModule(device_, vert_mod, nullptr);

    FJELL_GFX_DEBUG("Bloom pass created");
}

void BloomPass::destroy() {
    invalidate_imgui_texture();
    destroy_mip_chain();

    auto d = [this](auto& h) { if (h != VK_NULL_HANDLE) { vkDestroyPipeline(device_, h, nullptr); h = VK_NULL_HANDLE; } };
    d(downsample_pipeline_); d(upsample_pipeline_); d(composite_pipeline_);
    auto dl = [this](auto& h) { if (h != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device_, h, nullptr); h = VK_NULL_HANDLE; } };
    dl(downsample_layout_); dl(upsample_layout_); dl(composite_layout_pipeline_);

    if (pool_ != VK_NULL_HANDLE) { vkDestroyDescriptorPool(device_, pool_, nullptr); pool_ = VK_NULL_HANDLE; }
    if (single_tex_layout_ != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device_, single_tex_layout_, nullptr); single_tex_layout_ = VK_NULL_HANDLE; }
    if (composite_layout_ != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device_, composite_layout_, nullptr); composite_layout_ = VK_NULL_HANDLE; }
    if (sampler_ != VK_NULL_HANDLE) { vkDestroySampler(device_, sampler_, nullptr); sampler_ = VK_NULL_HANDLE; }
}

// ── Mip chain ───────────────────────────────────────────────────────────

void BloomPass::create_mip_chain(uint32_t width, uint32_t height) {
    extent_ = {width, height};
    VkFormat fmt = VK_FORMAT_R16G16B16A16_SFLOAT;

    auto create_img = [&](uint32_t w, uint32_t h) -> MipLevel {
        MipLevel mip{};
        mip.width = w;
        mip.height = h;

        VkImageCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = fmt;
        ci.extent = {w, h, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(device_, &ci, nullptr, &mip.image) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create bloom mip image");
        }

        VkMemoryRequirements mem_reqs;
        vkGetImageMemoryRequirements(device_, mip.image, &mem_reqs);

        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = mem_reqs.size;
        alloc.memoryTypeIndex = Buffer::find_memory_type(
            physical_device_, mem_reqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(device_, &alloc, nullptr, &mip.memory) != VK_SUCCESS) {
            throw std::runtime_error("Failed to allocate bloom mip memory");
        }
        vkBindImageMemory(device_, mip.image, mip.memory, 0);

        VkImageViewCreateInfo view_ci{};
        view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_ci.image = mip.image;
        view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_ci.format = fmt;
        view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device_, &view_ci, nullptr, &mip.view) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create bloom mip view");
        }

        return mip;
    };

    uint32_t w = width / 2;
    uint32_t h = height / 2;
    for (uint32_t i = 0; i < MIP_COUNT; ++i) {
        mips_[i] = create_img(std::max(w, 1u), std::max(h, 1u));
        w /= 2;
        h /= 2;
    }

    // Output image (full resolution)
    VkImageCreateInfo out_ci{};
    out_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    out_ci.imageType = VK_IMAGE_TYPE_2D;
    out_ci.format = fmt;
    out_ci.extent = {width, height, 1};
    out_ci.mipLevels = 1;
    out_ci.arrayLayers = 1;
    out_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    out_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    out_ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    out_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(device_, &out_ci, nullptr, &output_image_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create bloom output image");
    }
    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(device_, output_image_, &mem_reqs);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = mem_reqs.size;
    alloc.memoryTypeIndex = Buffer::find_memory_type(
        physical_device_, mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device_, &alloc, nullptr, &output_memory_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate bloom output memory");
    }
    vkBindImageMemory(device_, output_image_, output_memory_, 0);

    VkImageViewCreateInfo out_view_ci{};
    out_view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    out_view_ci.image = output_image_;
    out_view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    out_view_ci.format = fmt;
    out_view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device_, &out_view_ci, nullptr, &output_view_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create bloom output view");
    }

    update_descriptors();
}

void BloomPass::destroy_mip_chain() {
    for (auto& mip : mips_) {
        if (mip.view != VK_NULL_HANDLE) vkDestroyImageView(device_, mip.view, nullptr);
        if (mip.image != VK_NULL_HANDLE) vkDestroyImage(device_, mip.image, nullptr);
        if (mip.memory != VK_NULL_HANDLE) vkFreeMemory(device_, mip.memory, nullptr);
        mip = {};
    }
    if (output_view_ != VK_NULL_HANDLE) { vkDestroyImageView(device_, output_view_, nullptr); output_view_ = VK_NULL_HANDLE; }
    if (output_image_ != VK_NULL_HANDLE) { vkDestroyImage(device_, output_image_, nullptr); output_image_ = VK_NULL_HANDLE; }
    if (output_memory_ != VK_NULL_HANDLE) { vkFreeMemory(device_, output_memory_, nullptr); output_memory_ = VK_NULL_HANDLE; }
}

void BloomPass::update_descriptors() {
    // mip_sets_[0..MIP_COUNT-1] = mip chain inputs (updated on resize only)
    for (uint32_t i = 0; i < MIP_COUNT; ++i) {
        VkDescriptorImageInfo img_info{};
        img_info.sampler = sampler_;
        img_info.imageView = mips_[i].view;
        img_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = mip_sets_[i];
        write.dstBinding = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &img_info;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }
}

void BloomPass::resize(uint32_t width, uint32_t height) {
    if (extent_.width == width && extent_.height == height) return;
    vkDeviceWaitIdle(device_);
    invalidate_imgui_texture();
    destroy_mip_chain();
    if (width > 0 && height > 0) {
        create_mip_chain(width, height);
    }
}

VkDescriptorSet BloomPass::imgui_texture() {
    if (imgui_descriptor_ == VK_NULL_HANDLE && output_view_ != VK_NULL_HANDLE) {
        imgui_descriptor_ = ImGui_ImplVulkan_AddTexture(
            sampler_, output_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return imgui_descriptor_;
}

void BloomPass::invalidate_imgui_texture() {
    if (imgui_descriptor_ != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(imgui_descriptor_);
        imgui_descriptor_ = VK_NULL_HANDLE;
    }
}

// ── Recording ───────────────────────────────────────────────────────────

static void transition(VkCommandBuffer cmd, VkImage image,
                       VkImageLayout old_layout, VkImageLayout new_layout,
                       VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                       VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

static void draw_fullscreen(VkCommandBuffer cmd, VkPipeline pipeline,
                            VkPipelineLayout layout, VkDescriptorSet set,
                            VkImageView target_view, uint32_t w, uint32_t h,
                            VkAttachmentLoadOp load_op,
                            const void* push_data, uint32_t push_size) {
    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = target_view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = load_op;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.extent = {w, h};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;

    vkCmdBeginRendering(cmd, &ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    VkViewport vp{};
    vp.width = static_cast<float>(w);
    vp.height = static_cast<float>(h);
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D sc{};
    sc.extent = {w, h};
    vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                            0, 1, &set, 0, nullptr);
    if (push_data && push_size > 0) {
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, push_size, push_data);
    }
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

void BloomPass::record(const FrameContext& ctx) {
    if (!ctx.target || output_view_ == VK_NULL_HANDLE) return;

    auto cmd = ctx.cmd;

    uint32_t fi = ctx.frame_index;

    // Update scene source descriptor (per-frame)
    {
        VkDescriptorImageInfo img_info{};
        img_info.sampler = sampler_;
        img_info.imageView = ctx.target->color_view();
        img_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = source_sets_[fi];
        write.dstBinding = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &img_info;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }

    // ── Downsample chain ────────────────────────────────────────────────

    for (uint32_t i = 0; i < MIP_COUNT; ++i) {
        auto& mip = mips_[i];
        VkDescriptorSet src_set = (i == 0) ? source_sets_[fi] : mip_sets_[i - 1];

        struct { float threshold, is_first, pad1, pad2; } push{
            threshold_, (i == 0) ? 1.0f : 0.0f, 0.0f, 0.0f};

        transition(cmd, mip.image,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        draw_fullscreen(cmd, downsample_pipeline_, downsample_layout_, src_set,
                        mip.view, mip.width, mip.height,
                        VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                        &push, sizeof(push));

        transition(cmd, mip.image,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }

    // ── Upsample chain (overwrite each mip with the upscaled smaller one) ──

    for (int i = static_cast<int>(MIP_COUNT) - 2; i >= 0; --i) {
        auto& dst = mips_[i];

        struct { float blend, pad1, pad2, pad3; } push{1.0f, 0.0f, 0.0f, 0.0f};

        transition(cmd, dst.image,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        // Source = the mip below (i+1)
        draw_fullscreen(cmd, upsample_pipeline_, upsample_layout_, mip_sets_[i + 1],
                        dst.view, dst.width, dst.height,
                        VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                        &push, sizeof(push));

        transition(cmd, dst.image,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }

    // ── Composite: scene + bloom → output ───────────────────────────────

    // Update composite descriptor: binding 0 = scene, binding 1 = bloom (mip 0)
    {
        std::array<VkDescriptorImageInfo, 2> imgs{};
        imgs[0].sampler = sampler_;
        imgs[0].imageView = ctx.target->color_view();
        imgs[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imgs[1].sampler = sampler_;
        imgs[1].imageView = mips_[0].view;
        imgs[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        std::array<VkWriteDescriptorSet, 2> writes{};
        for (int b = 0; b < 2; ++b) {
            writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[b].dstSet = composite_sets_[fi];
            writes[b].dstBinding = b;
            writes[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[b].descriptorCount = 1;
            writes[b].pImageInfo = &imgs[b];
        }
        vkUpdateDescriptorSets(device_, 2, writes.data(), 0, nullptr);
    }

    struct { float intensity, p1, p2, p3; } comp_push{intensity_, 0, 0, 0};

    transition(cmd, output_image_,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    draw_fullscreen(cmd, composite_pipeline_, composite_layout_pipeline_,
                    composite_sets_[fi],
                    output_view_, extent_.width, extent_.height,
                    VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                    &comp_push, sizeof(comp_push));

    transition(cmd, output_image_,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

} // namespace fjell
