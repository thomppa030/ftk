#include "renderer/swapchain.hpp"
#include "renderer/buffer.hpp"
#include "renderer/device.hpp"
#include "core/log.hpp"
#include "core/window.hpp"

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace fjell {

Swapchain::Swapchain(Device& device, Window& window)
    : device_{device}, window_{window} {
    create_swapchain();
    create_image_views();
    create_depth_resources();
    create_render_finished_semaphores();
}

Swapchain::~Swapchain() {
    auto vk_device = device_.handle();

    for (auto sem : render_finished_)
        vkDestroySemaphore(vk_device, sem, nullptr);

    cleanup();
}

void Swapchain::cleanup() {
    auto vk_device = device_.handle();

    if (depth_image_view_ != VK_NULL_HANDLE)
        vkDestroyImageView(vk_device, depth_image_view_, nullptr);
    if (depth_image_ != VK_NULL_HANDLE)
        vkDestroyImage(vk_device, depth_image_, nullptr);
    if (depth_memory_ != VK_NULL_HANDLE)
        vkFreeMemory(vk_device, depth_memory_, nullptr);
    depth_image_view_ = VK_NULL_HANDLE;
    depth_image_ = VK_NULL_HANDLE;
    depth_memory_ = VK_NULL_HANDLE;

    for (auto view : image_views_)
        vkDestroyImageView(vk_device, view, nullptr);
    image_views_.clear();

    if (swapchain_ != VK_NULL_HANDLE)
        vkDestroySwapchainKHR(vk_device, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

void Swapchain::recreate() {
    int width = 0, height = 0;
    glfwGetFramebufferSize(window_.handle(), &width, &height);
    while (width == 0 || height == 0) {
        glfwGetFramebufferSize(window_.handle(), &width, &height);
        glfwWaitEvents();
    }

    vkDeviceWaitIdle(device_.handle());

    for (auto sem : render_finished_)
        vkDestroySemaphore(device_.handle(), sem, nullptr);
    render_finished_.clear();

    cleanup();

    create_swapchain();
    create_image_views();
    create_depth_resources();
    create_render_finished_semaphores();

}

void Swapchain::create_swapchain() {
    auto support = device_.query_swapchain_support();
    auto format = choose_surface_format(support.formats);
    auto mode = choose_present_mode(support.present_modes);
    auto extent = choose_extent(support.capabilities);

    uint32_t image_count = support.capabilities.minImageCount + 1;
    if (support.capabilities.maxImageCount > 0 &&
        image_count > support.capabilities.maxImageCount) {
        image_count = support.capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    create_info.surface = device_.surface();
    create_info.minImageCount = image_count;
    create_info.imageFormat = format.format;
    create_info.imageColorSpace = format.colorSpace;
    create_info.imageExtent = extent;
    create_info.imageArrayLayers = 1;
    create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    auto indices = device_.find_queue_families();
    uint32_t family_indices[] = {indices.graphics.value(), indices.present.value()};

    if (indices.graphics != indices.present) {
        create_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        create_info.queueFamilyIndexCount = 2;
        create_info.pQueueFamilyIndices = family_indices;
    } else {
        create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    create_info.preTransform = support.capabilities.currentTransform;
    create_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    create_info.presentMode = mode;
    create_info.clipped = VK_TRUE;

    if (vkCreateSwapchainKHR(device_.handle(), &create_info, nullptr, &swapchain_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create swapchain");
    }

    vkGetSwapchainImagesKHR(device_.handle(), swapchain_, &image_count, nullptr);
    images_.resize(image_count);
    vkGetSwapchainImagesKHR(device_.handle(), swapchain_, &image_count, images_.data());

    format_ = format.format;
    extent_ = extent;

    FJELL_GFX_DEBUG("Swapchain created: {}x{}, {} images",
                    extent.width, extent.height, images_.size());
}

void Swapchain::create_image_views() {
    image_views_.resize(images_.size());

    for (size_t i = 0; i < images_.size(); ++i) {
        VkImageViewCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        create_info.image = images_[i];
        create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        create_info.format = format_;
        create_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        create_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        create_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        create_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        create_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        create_info.subresourceRange.baseMipLevel = 0;
        create_info.subresourceRange.levelCount = 1;
        create_info.subresourceRange.baseArrayLayer = 0;
        create_info.subresourceRange.layerCount = 1;

        if (vkCreateImageView(device_.handle(), &create_info, nullptr, &image_views_[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create image view");
        }
    }
}

void Swapchain::create_depth_resources() {
    VkFormat depth_format = device_.find_depth_format();
    auto vk_device = device_.handle();

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.extent.width = extent_.width;
    image_info.extent.height = extent_.height;
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.format = depth_format;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(vk_device, &image_info, nullptr, &depth_image_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create depth image");
    }

    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(vk_device, depth_image_, &mem_reqs);

    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex = Buffer::find_memory_type(
        device_.physical_device(), mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(vk_device, &alloc_info, nullptr, &depth_memory_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate depth image memory");
    }

    vkBindImageMemory(vk_device, depth_image_, depth_memory_, 0);

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = depth_image_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = depth_format;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;

    if (vkCreateImageView(vk_device, &view_info, nullptr, &depth_image_view_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create depth image view");
    }
}

void Swapchain::create_render_finished_semaphores() {
    VkSemaphoreCreateInfo sem_info{};
    sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    render_finished_.resize(images_.size());
    for (auto& sem : render_finished_) {
        if (vkCreateSemaphore(device_.handle(), &sem_info, nullptr, &sem) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create render finished semaphore");
        }
    }
}

VkSurfaceFormatKHR Swapchain::choose_surface_format(
    const std::vector<VkSurfaceFormatKHR>& formats) const {
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return f;
        }
    }
    return formats[0];
}

VkPresentModeKHR Swapchain::choose_present_mode(
    const std::vector<VkPresentModeKHR>& modes) const {
    for (const auto& mode : modes) {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR) return mode;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D Swapchain::choose_extent(
    const VkSurfaceCapabilitiesKHR& capabilities) const {
    if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        return capabilities.currentExtent;
    }

    VkExtent2D extent = {window_.width(), window_.height()};
    extent.width = std::clamp(extent.width,
        capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
    extent.height = std::clamp(extent.height,
        capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    return extent;
}

} // namespace fjell
