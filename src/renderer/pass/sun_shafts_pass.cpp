#include "renderer/pass/sun_shafts_pass.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/gpu/vma_image.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/resources/shader_utils.hpp"
#include "renderer/resource_registry.hpp"
#include "scene/light_node.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace fjell {

struct ShaftsPushConstants {
    glm::uvec2 output_size;
    uint32_t frame_index;
    float pad0;
};

namespace {

VkPipeline create_compute_pipeline(VkDevice device, VkPipelineLayout layout,
                                   const std::string& shader_path,
                                   VkPipelineCache pipeline_cache) {
    auto code = read_spirv(base_path::resolve(shader_path));
    auto module = create_shader_module(device, code);

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName = "main";

    VkComputePipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage = stage;
    ci.layout = layout;

    VkPipeline pipeline;
    if (vkCreateComputePipelines(device, pipeline_cache, 1, &ci, nullptr, &pipeline) != VK_SUCCESS) {
        vkDestroyShaderModule(device, module, nullptr);
        throw std::runtime_error("Failed to create sun shafts compute pipeline");
    }
    vkDestroyShaderModule(device, module, nullptr);
    return pipeline;
}

} // namespace

// ── Image helpers ──────────────────────────────────────────────────────────

SunShaftsPass::Image SunShaftsPass::create_image(VkDevice device, VmaAllocator allocator,
                                                 uint32_t w, uint32_t h) {
    Image img{};

    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = SHAFTS_FORMAT;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (create_vma_image(allocator, &ci, &img.image, &img.memory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create sun shafts image");
    }

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = SHAFTS_FORMAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device, &vi, nullptr, &img.view) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create sun shafts image view");
    }

    return img;
}

void SunShaftsPass::destroy_image(VkDevice device, VmaAllocator allocator, Image& img) {
    if (img.view) vkDestroyImageView(device, img.view, nullptr);
    destroy_vma_image(allocator, img.image, img.memory);
    img = {};
}

// ── Images ─────────────────────────────────────────────────────────────────

void SunShaftsPass::Images::create(VkDevice device, VmaAllocator allocator,
                                   VkDescriptorSetLayout march_layout,
                                   uint32_t screen_w, uint32_t screen_h) {
    uint32_t w = std::max(1u, screen_w / RESOLUTION_DIVISOR);
    uint32_t h = std::max(1u, screen_h / RESOLUTION_DIVISOR);

    extent = {w, h};
    frame_counter = 0;

    shafts = create_image(device, allocator, w, h);
    shafts_history = create_image(device, allocator, w, h);

    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1};
    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.maxSets = 1;
    pool_ci.poolSizeCount = 1;
    pool_ci.pPoolSizes = &pool_size;
    vkCreateDescriptorPool(device, &pool_ci, nullptr, &pool);

    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = pool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &march_layout;
    vkAllocateDescriptorSets(device, &alloc, &march_set);

    VkDescriptorImageInfo storage_info{};
    storage_info.imageView = shafts.view;
    storage_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = march_set;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.descriptorCount = 1;
    write.pImageInfo = &storage_info;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}

void SunShaftsPass::Images::destroy(VkDevice device, VmaAllocator allocator) {
    if (pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, pool, nullptr);
        pool = VK_NULL_HANDLE;
        march_set = VK_NULL_HANDLE;
    }
    destroy_image(device, allocator, shafts);
    destroy_image(device, allocator, shafts_history);
    extent = {};
}

void SunShaftsPass::Images::resize(VkDevice device, VmaAllocator allocator,
                                   VkDescriptorSetLayout march_layout,
                                   uint32_t screen_w, uint32_t screen_h) {
    uint32_t w = std::max(1u, screen_w / RESOLUTION_DIVISOR);
    uint32_t h = std::max(1u, screen_h / RESOLUTION_DIVISOR);
    if (w == extent.width && h == extent.height) return;
    destroy(device, allocator);
    create(device, allocator, march_layout, screen_w, screen_h);
}

// ── SunShaftsPass ──────────────────────────────────────────────────────────

Result<> SunShaftsPass::create(Device& device, const PassResources& resources) {
    device_ = device.handle();
    vk_pipeline_cache_ = resources.pipeline_cache;
    allocator_ = resources.allocator;

    // March set layout: binding 0 = storage image (half-res shaft output)
    {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ci.bindingCount = 1;
        ci.pBindings = &binding;
        vkCreateDescriptorSetLayout(device_, &ci, nullptr, &march_set_layout_);

        VkPushConstantRange push_range{};
        push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push_range.offset = 0;
        push_range.size = sizeof(ShaftsPushConstants);

        VkPipelineLayoutCreateInfo pl_ci{};
        pl_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pl_ci.setLayoutCount = 1;
        pl_ci.pSetLayouts = &march_set_layout_;
        pl_ci.pushConstantRangeCount = 1;
        pl_ci.pPushConstantRanges = &push_range;
        if (vkCreatePipelineLayout(device_, &pl_ci, nullptr, &march_pipeline_layout_) != VK_SUCCESS) {
            return make_error("Failed to create sun shafts pipeline layout");
        }

        march_pipeline_ = create_compute_pipeline(
            device_, march_pipeline_layout_, "shaders/sun_shafts.comp.spv", vk_pipeline_cache_);
    }

    default_images_.create(device_, allocator_, march_set_layout_, 2, 2);

    FJELL_GFX_DEBUG("Sun shafts pass created");
    return {};
}

void SunShaftsPass::destroy() {
    if (!device_) return;
    default_images_.destroy(device_, allocator_);
    if (march_pipeline_) { vkDestroyPipeline(device_, march_pipeline_, nullptr); march_pipeline_ = VK_NULL_HANDLE; }
    if (march_pipeline_layout_) { vkDestroyPipelineLayout(device_, march_pipeline_layout_, nullptr); march_pipeline_layout_ = VK_NULL_HANDLE; }
    if (march_set_layout_) { vkDestroyDescriptorSetLayout(device_, march_set_layout_, nullptr); march_set_layout_ = VK_NULL_HANDLE; }
    device_ = VK_NULL_HANDLE;
}

void SunShaftsPass::prepare_frame(const FrameContext& ctx, GpuContext& /*gpu*/) {
    enabled_ = false;
    if (!ctx.scene) return;

    for (const auto& obj : ctx.scene->objects()) {
        if (const auto* fog = dynamic_cast<const VolumetricFogNode*>(obj.get())) {
            enabled_ = fog->shafts_enabled;
            intensity_ = fog->shaft_intensity;
            density_mult_ = fog->shaft_density_mult;
            max_reach_ = fog->shaft_max_reach;
            step_count_ = fog->shaft_step_count;
            anisotropy_ = fog->shaft_anisotropy;
            fog_density_ = fog->density;
            break;
        }
    }
}

PassSetup SunShaftsPass::setup() const {
    return {
        .name = "sun_shafts",
        .phase = RenderPhase::PostProcess,
        .sub_order = 120,  // after volumetric_fog (100), before taa (150)
        .reads = {{"depth", ImageUsage::compute_read}},
        .writes = {},  // phase 1: writes only to our own half-res image, not yet consumed
    };
}

void SunShaftsPass::record(const FrameContext& ctx) {
    if (!enabled_ || !ctx.target) return;

    auto extent = ctx.target->extent();
    default_images_.resize(device_, allocator_, march_set_layout_,
                           extent.width, extent.height);

    VkImageMemoryBarrier to_general{};
    to_general.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_general.oldLayout = default_images_.frame_counter == 0
        ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
    to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    to_general.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    to_general.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    to_general.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_general.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_general.image = default_images_.shafts.image;
    to_general.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(ctx.cmd,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &to_general);

    ShaftsPushConstants pc{};
    pc.output_size = glm::uvec2(default_images_.extent.width, default_images_.extent.height);
    pc.frame_index = default_images_.frame_counter;

    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, march_pipeline_);
    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            march_pipeline_layout_, 0, 1, &default_images_.march_set,
                            0, nullptr);
    vkCmdPushConstants(ctx.cmd, march_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(pc), &pc);

    uint32_t groups_x = (default_images_.extent.width + TILE_SIZE - 1) / TILE_SIZE;
    uint32_t groups_y = (default_images_.extent.height + TILE_SIZE - 1) / TILE_SIZE;
    vkCmdDispatch(ctx.cmd, groups_x, groups_y, 1);

    ++default_images_.frame_counter;
}

} // namespace fjell
