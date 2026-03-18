#include "ui/icon_cache.hpp"
#include "core/log.hpp"
#include "renderer/vk_check.hpp"

#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <stb_image.h>

#include <filesystem>

namespace fjell {

namespace {

uint32_t find_memory_type(VkPhysicalDevice physical_device,
                          uint32_t type_filter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((type_filter & (1 << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    throw std::runtime_error("failed to find suitable memory type for icon");
}

} // namespace

IconCache::IconCache(VkDevice device, VkPhysicalDevice physical_device,
                     VkCommandPool command_pool, VkQueue queue,
                     const std::string& icons_dir)
    : device_{device}
    , physical_device_{physical_device}
    , command_pool_{command_pool}
    , queue_{queue}
{
    namespace fs = std::filesystem;
    if (!fs::is_directory(icons_dir)) {
        FJELL_CORE_WARN("Icons directory not found: {}", icons_dir);
        return;
    }

    for (const auto& entry : fs::directory_iterator(icons_dir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".png") continue;

        auto filename = entry.path().stem().string();
        // Strip "icon_" prefix
        if (filename.starts_with("icon_")) {
            filename = filename.substr(5);
        }
        load_icon(filename, entry.path().string());
    }

    FJELL_CORE_INFO("Loaded {} icons", icons_.size());
}

IconCache::~IconCache() {
    vkDeviceWaitIdle(device_);
    for (auto& [name, e] : icons_) {
        if (e.descriptor != VK_NULL_HANDLE) {
            ImGui_ImplVulkan_RemoveTexture(e.descriptor);
        }
        if (e.sampler != VK_NULL_HANDLE) vkDestroySampler(device_, e.sampler, nullptr);
        if (e.view != VK_NULL_HANDLE) vkDestroyImageView(device_, e.view, nullptr);
        if (e.image != VK_NULL_HANDLE) vkDestroyImage(device_, e.image, nullptr);
        if (e.memory != VK_NULL_HANDLE) vkFreeMemory(device_, e.memory, nullptr);
    }
}

VkDescriptorSet IconCache::icon(const std::string& name) const {
    auto it = icons_.find(name);
    return it != icons_.end() ? it->second.descriptor : VK_NULL_HANDLE;
}

void IconCache::load_icon(const std::string& name, const std::string& path) {
    int w, h, channels;
    auto* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!pixels) {
        FJELL_CORE_WARN("Failed to load icon: {}", path);
        return;
    }

    VkDeviceSize image_size = static_cast<VkDeviceSize>(w) * h * 4;

    // Staging buffer
    VkBuffer staging_buffer;
    VkDeviceMemory staging_memory;

    VkBufferCreateInfo buf_info{};
    buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_info.size = image_size;
    buf_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vk_check(vkCreateBuffer(device_, &buf_info, nullptr, &staging_buffer),
             "create icon staging buffer");

    VkMemoryRequirements mem_req;
    vkGetBufferMemoryRequirements(device_, staging_buffer, &mem_req);

    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_req.size;
    alloc_info.memoryTypeIndex = find_memory_type(
        physical_device_, mem_req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vk_check(vkAllocateMemory(device_, &alloc_info, nullptr, &staging_memory),
             "allocate icon staging memory");
    vk_check(vkBindBufferMemory(device_, staging_buffer, staging_memory, 0),
             "bind icon staging memory");

    void* data;
    vk_check(vkMapMemory(device_, staging_memory, 0, image_size, 0, &data),
             "map icon staging memory");
    std::memcpy(data, pixels, image_size);
    vkUnmapMemory(device_, staging_memory);
    stbi_image_free(pixels);

    // Create image
    IconEntry entry{};

    VkImageCreateInfo img_info{};
    img_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_info.imageType = VK_IMAGE_TYPE_2D;
    img_info.extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
    img_info.mipLevels = 1;
    img_info.arrayLayers = 1;
    img_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    img_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    img_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    img_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    img_info.samples = VK_SAMPLE_COUNT_1_BIT;
    vk_check(vkCreateImage(device_, &img_info, nullptr, &entry.image),
             "create icon image");

    vkGetImageMemoryRequirements(device_, entry.image, &mem_req);
    alloc_info.allocationSize = mem_req.size;
    alloc_info.memoryTypeIndex = find_memory_type(
        physical_device_, mem_req.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vk_check(vkAllocateMemory(device_, &alloc_info, nullptr, &entry.memory),
             "allocate icon image memory");
    vk_check(vkBindImageMemory(device_, entry.image, entry.memory, 0),
             "bind icon image memory");

    // Transition + copy via one-shot command buffer
    VkCommandBufferAllocateInfo cmd_alloc{};
    cmd_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_alloc.commandPool = command_pool_;
    cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_alloc.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vk_check(vkAllocateCommandBuffers(device_, &cmd_alloc, &cmd),
             "allocate icon cmd");

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(cmd, &begin), "begin icon cmd");

    // Transition to transfer dst
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = entry.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
    vkCmdCopyBufferToImage(cmd, staging_buffer, entry.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Transition to shader read
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    vk_check(vkEndCommandBuffer(cmd), "end icon cmd");

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vk_check(vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE), "submit icon cmd");
    vk_check(vkQueueWaitIdle(queue_), "wait icon upload");

    vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
    vkDestroyBuffer(device_, staging_buffer, nullptr);
    vkFreeMemory(device_, staging_memory, nullptr);

    // Image view
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = entry.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk_check(vkCreateImageView(device_, &view_info, nullptr, &entry.view),
             "create icon image view");

    // Sampler
    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vk_check(vkCreateSampler(device_, &sampler_info, nullptr, &entry.sampler),
             "create icon sampler");

    // ImGui descriptor
    entry.descriptor = ImGui_ImplVulkan_AddTexture(
        entry.sampler, entry.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    icons_[name] = entry;
}

} // namespace fjell
