#include "renderer/pass/ddgi_pass.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/gpu/vma_image.hpp"

#include <algorithm>
#include <cmath>

namespace fjell {

// ── Images ──────────────────────────────────────────────────────────────

static void compute_atlas_dims(const DdgiConfig& config,
                                int32_t& irr_w, int32_t& irr_h,
                                int32_t& vis_w, int32_t& vis_h,
                                int32_t& total) {
    total = config.probe_counts.x * config.probe_counts.y * config.probe_counts.z;
    int probes_per_row = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(total))));

    int irr_side_with_border = config.irradiance_side_length + 2;
    irr_w = probes_per_row * irr_side_with_border;
    irr_h = ((total + probes_per_row - 1) / probes_per_row) * irr_side_with_border;

    int vis_side_with_border = config.visibility_side_length + 2;
    vis_w = probes_per_row * vis_side_with_border;
    vis_h = ((total + probes_per_row - 1) / probes_per_row) * vis_side_with_border;
}

void DdgiPass::Images::create(VkDevice device, VmaAllocator allocator,
                               const DdgiConfig& config,
                               uint32_t screen_w, uint32_t screen_h) {
    compute_atlas_dims(config, irr_width, irr_height, vis_width, vis_height, total_probes);

    auto create_image = [&](VkImage& img, VmaAllocation& alloc, VkImageView& view,
                            uint32_t w, uint32_t h, VkFormat format,
                            VkImageUsageFlags usage) {
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {w, h, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        create_vma_image(allocator, &info, &img, &alloc);

        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = img;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = format;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        vkCreateImageView(device, &view_info, nullptr, &view);
    };

    VkImageUsageFlags storage_sampled = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    // Radiance: probe_rays x total_probes
    create_image(radiance_image, radiance_memory, radiance_view,
                 static_cast<uint32_t>(config.probe_rays),
                 static_cast<uint32_t>(total_probes),
                 VK_FORMAT_R16G16B16A16_SFLOAT, storage_sampled);

    // Irradiance atlas
    create_image(irradiance_image, irradiance_memory, irradiance_view,
                 static_cast<uint32_t>(irr_width),
                 static_cast<uint32_t>(irr_height),
                 VK_FORMAT_R16G16B16A16_SFLOAT, storage_sampled);

    // Visibility atlas
    create_image(visibility_image, visibility_memory, visibility_view,
                 static_cast<uint32_t>(vis_width),
                 static_cast<uint32_t>(vis_height),
                 VK_FORMAT_R16G16_SFLOAT, storage_sampled);

    // Probe offsets: total_probes stored as 2D (probes_xy x probes_z)
    int probes_xy = config.probe_counts.x * config.probe_counts.y;
    create_image(probe_offset_image, probe_offset_memory, probe_offset_view,
                 static_cast<uint32_t>(probes_xy),
                 static_cast<uint32_t>(config.probe_counts.z),
                 VK_FORMAT_R16G16B16A16_SFLOAT, storage_sampled);

    // Probe status SSBO
    VkDeviceSize status_size = static_cast<VkDeviceSize>(total_probes) * sizeof(uint32_t);
    probe_status_buffer = std::make_unique<Buffer>(
        allocator, status_size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO);

    // Constants UBO
    constants_ubo = std::make_unique<Buffer>(
        allocator, sizeof(GpuDdgiConstants),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    constants_ubo->map();

    // Indirect output (screen-res or half-res)
    uint32_t out_w = config.half_res_output ? std::max(1u, screen_w / 2) : screen_w;
    uint32_t out_h = config.half_res_output ? std::max(1u, screen_h / 2) : screen_h;
    indirect_extent = {out_w, out_h};

    create_image(indirect_image, indirect_memory, indirect_view,
                 out_w, out_h,
                 VK_FORMAT_R16G16B16A16_SFLOAT, storage_sampled);

    images_need_init = true;
}

void DdgiPass::Images::destroy(VkDevice device, VmaAllocator allocator) {
    auto destroy_img = [&](VkImageView& view, VkImage& img, VmaAllocation& alloc) {
        if (view != VK_NULL_HANDLE) vkDestroyImageView(device, view, nullptr);
        destroy_vma_image(allocator, img, alloc);
        view = VK_NULL_HANDLE;
        img = VK_NULL_HANDLE;
        alloc = VK_NULL_HANDLE;
    };

    destroy_img(radiance_view, radiance_image, radiance_memory);
    destroy_img(irradiance_view, irradiance_image, irradiance_memory);
    destroy_img(visibility_view, visibility_image, visibility_memory);
    destroy_img(probe_offset_view, probe_offset_image, probe_offset_memory);
    destroy_img(indirect_view, indirect_image, indirect_memory);

    probe_status_buffer.reset();
    constants_ubo.reset();
}

void DdgiPass::Images::resize(VkDevice device, VmaAllocator allocator,
                               const DdgiConfig& config,
                               uint32_t screen_w, uint32_t screen_h) {
    destroy(device, allocator);
    create(device, allocator, config, screen_w, screen_h);
}

void DdgiPass::Images::invalidate_imgui_texture() {
    // No imgui textures yet — will add debug viz later
}

// ── DdgiPass ────────────────────────────────────────────────────────────

Result<> DdgiPass::create(Device& device, const PassResources& /*resources*/) {
    device_ = &device;
    // Compute pipelines will be created in Phases 3-5
    return {};
}

void DdgiPass::destroy() {
    // Compute pipelines will be destroyed in Phases 3-5
    device_ = nullptr;
}

void DdgiPass::record(const FrameContext& /*ctx*/) {
    // Stub — DDGI uses record_into() with per-viewport Images
}

void DdgiPass::record_into(const FrameContext& /*ctx*/, Images& /*imgs*/) {
    // Stub — compute dispatches added in Phases 3-5
    frame_counter_++;
}

} // namespace fjell
