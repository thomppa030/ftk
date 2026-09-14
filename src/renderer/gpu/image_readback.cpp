#include "renderer/gpu/image_readback.hpp"

#include "renderer/gpu/vk_check.hpp"
#include "renderer/gpu/vk_utils.hpp"

namespace fjell {

ImageReadback::ImageReadback(const Queue& queue, VkImage source, VkImageLayout source_layout,
                             uint32_t width, uint32_t height)
    : allocator_{queue.allocator}
    , width_{width}
    , height_{height}
{
    // Host-visible staging buffer the pixels end up in.
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = static_cast<VkDeviceSize>(width) * height * 4;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo buffer_alloc{};
    buffer_alloc.usage = VMA_MEMORY_USAGE_AUTO;
    buffer_alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                         VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo alloc_info{};
    vk_check(vmaCreateBuffer(allocator_, &buffer_info, &buffer_alloc,
                             &buffer_, &allocation_, &alloc_info),
             "create readback staging buffer");
    mapped_ = alloc_info.pMappedData;

    // Transient 8-bit sRGB image the source is blitted through. The blit
    // converts format and the copy then reads a layout every format can
    // land in.
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_SRGB;
    image_info.extent = {width, height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo image_alloc{};
    image_alloc.usage = VMA_MEMORY_USAGE_AUTO;
    VkImage blit_image{VK_NULL_HANDLE};
    VmaAllocation blit_allocation{VK_NULL_HANDLE};
    vk_check(vmaCreateImage(allocator_, &image_info, &image_alloc,
                            &blit_image, &blit_allocation, nullptr),
             "create readback blit image");

    VkCommandBufferAllocateInfo cmd_alloc{};
    cmd_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_alloc.commandPool = queue.command_pool;
    cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_alloc.commandBufferCount = 1;
    VkCommandBuffer cmd{VK_NULL_HANDLE};
    vk_check(vkAllocateCommandBuffers(queue.device, &cmd_alloc, &cmd),
             "allocate readback command buffer");

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(cmd, &begin), "begin readback command buffer");

    vk_utils::transition_image(cmd, source,
        source_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);

    vk_utils::transition_image(cmd, blit_image,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
        VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

    VkImageBlit region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffsets[1] = {static_cast<int32_t>(width), static_cast<int32_t>(height), 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstOffsets[1] = {static_cast<int32_t>(width), static_cast<int32_t>(height), 1};
    vkCmdBlitImage(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   blit_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &region, VK_FILTER_NEAREST);

    vk_utils::transition_image(cmd, source,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, source_layout,
        VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT);

    vk_utils::transition_image(cmd, blit_image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);

    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(cmd, blit_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           buffer_, 1, &copy);

    vk_check(vkEndCommandBuffer(cmd), "end readback command buffer");

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vk_check(vkQueueSubmit(queue.queue, 1, &submit, VK_NULL_HANDLE), "submit readback");
    vk_check(vkQueueWaitIdle(queue.queue), "wait for readback");

    vkFreeCommandBuffers(queue.device, queue.command_pool, 1, &cmd);
    vmaDestroyImage(allocator_, blit_image, blit_allocation);
}

ImageReadback::~ImageReadback() {
    if (buffer_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator_, buffer_, allocation_);
    }
}

} // namespace fjell
