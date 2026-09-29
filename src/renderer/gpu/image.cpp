#include "renderer/gpu/image.hpp"
#include "gpu/vulkan/native.hpp"

#include <stdexcept>
#include <utility>

namespace fjell {

Image::Image(VmaAllocator allocator, uint32_t width, uint32_t height,
             VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage,
             VmaMemoryUsage memory_usage, uint32_t mip_levels)
    : allocator_{allocator}, width_{width}, height_{height} {
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.extent.width = width;
    image_info.extent.height = height;
    image_info.extent.depth = 1;
    image_info.mipLevels = mip_levels;
    image_info.arrayLayers = 1;
    image_info.format = format;
    image_info.tiling = tiling;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_info.usage = usage;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo alloc_ci{};
    alloc_ci.usage = memory_usage;
    alloc_ci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

    if (vmaCreateImage(allocator_, &image_info, &alloc_ci,
                       &image_, &allocation_, nullptr) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create image");
    }
    made_as_ = image_info;
}

Image::~Image() {
    if (view_ != VK_NULL_HANDLE)
        vkDestroyImageView(device_, view_, nullptr);
    if (image_ != VK_NULL_HANDLE)
        vmaDestroyImage(allocator_, image_, allocation_);
}

Image::Image(Image&& other) noexcept
    : allocator_{other.allocator_}
    , device_{other.device_}
    , image_{std::exchange(other.image_, VK_NULL_HANDLE)}
    , allocation_{std::exchange(other.allocation_, VK_NULL_HANDLE)}
    , view_{std::exchange(other.view_, VK_NULL_HANDLE)}
    , width_{other.width_}
    , height_{other.height_}
    , made_as_{other.made_as_}
    , shared_{std::move(other.shared_)} {
}

Image& Image::operator=(Image&& other) noexcept {
    if (this != &other) {
        if (view_ != VK_NULL_HANDLE) vkDestroyImageView(device_, view_, nullptr);
        if (image_ != VK_NULL_HANDLE) vmaDestroyImage(allocator_, image_, allocation_);

        allocator_ = other.allocator_;
        device_ = other.device_;
        image_ = std::exchange(other.image_, VK_NULL_HANDLE);
        allocation_ = std::exchange(other.allocation_, VK_NULL_HANDLE);
        view_ = std::exchange(other.view_, VK_NULL_HANDLE);
        width_ = other.width_;
        height_ = other.height_;
        made_as_ = other.made_as_;
        shared_ = std::move(other.shared_);
    }
    return *this;
}

void Image::share(gpu::Device& device) {
    shared_ = gpu::vulkan::adopt(device, image_, VK_NULL_HANDLE, made_as_);
}

Result<Image> Image::create(VmaAllocator allocator, uint32_t width, uint32_t height,
                            VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage,
                            VmaMemoryUsage memory_usage, uint32_t mip_levels) {
    Image img{};
    img.allocator_ = allocator;
    img.width_ = width;
    img.height_ = height;

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.extent.width = width;
    image_info.extent.height = height;
    image_info.extent.depth = 1;
    image_info.mipLevels = mip_levels;
    image_info.arrayLayers = 1;
    image_info.format = format;
    image_info.tiling = tiling;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_info.usage = usage;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo alloc_ci{};
    alloc_ci.usage = memory_usage;
    alloc_ci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

    if (vmaCreateImage(allocator, &image_info, &alloc_ci,
                       &img.image_, &img.allocation_, nullptr) != VK_SUCCESS) {
        return make_error("Failed to create image");
    }
    img.made_as_ = image_info;
    return img;
}

void Image::create_view(VkDevice device, VkFormat format, VkImageAspectFlags aspect,
                        uint32_t mip_levels) {
    device_ = device;
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange.aspectMask = aspect;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = mip_levels;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device_, &view_info, nullptr, &view_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create image view");
    }
}

Result<> Image::create_view_checked(VkDevice device, VkFormat format,
                                    VkImageAspectFlags aspect, uint32_t mip_levels) {
    device_ = device;
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange.aspectMask = aspect;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = mip_levels;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device_, &view_info, nullptr, &view_) != VK_SUCCESS) {
        return make_error("Failed to create image view");
    }
    return {};
}

} // namespace fjell
