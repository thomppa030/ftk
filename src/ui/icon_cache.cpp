#include "ui/icon_cache.hpp"
#include "core/log.hpp"
#include "core/thread_pool.hpp"
#include "renderer/vk_check.hpp"

#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <stb_image.h>

#include <stb_image_resize2.h>
#include <stb_image_write.h>

#include <cstring>
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
        if (filename.starts_with("icon_")) {
            filename = filename.substr(5);
        }
        load_icon(filename, entry.path().string());
    }

    FJELL_CORE_INFO("Loaded {} icons", icons_.size());
}

static void destroy_entry(VkDevice device, IconCache::IconEntry& e) {
    if (e.descriptor) ImGui_ImplVulkan_RemoveTexture(e.descriptor);
    if (e.sampler) vkDestroySampler(device, e.sampler, nullptr);
    if (e.view) vkDestroyImageView(device, e.view, nullptr);
    if (e.image) vkDestroyImage(device, e.image, nullptr);
    if (e.memory) vkFreeMemory(device, e.memory, nullptr);
    e = {};
}

IconCache::~IconCache() {
    vkDeviceWaitIdle(device_);
    for (auto& [name, e] : icons_) destroy_entry(device_, e);
    for (auto& [path, e] : thumbnails_) destroy_entry(device_, e);
}

VkDescriptorSet IconCache::icon(const std::string& name) const {
    auto it = icons_.find(name);
    return it != icons_.end() ? it->second.descriptor : VK_NULL_HANDLE;
}

// ── Shared upload ───────────────────────────────────────────────────────

IconCache::IconEntry IconCache::upload_rgba(const uint8_t* pixels, int w, int h) {
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
             "create staging buffer");

    VkMemoryRequirements mem_req;
    vkGetBufferMemoryRequirements(device_, staging_buffer, &mem_req);

    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_req.size;
    alloc_info.memoryTypeIndex = find_memory_type(
        physical_device_, mem_req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vk_check(vkAllocateMemory(device_, &alloc_info, nullptr, &staging_memory),
             "allocate staging memory");
    vk_check(vkBindBufferMemory(device_, staging_buffer, staging_memory, 0),
             "bind staging memory");

    void* data;
    vk_check(vkMapMemory(device_, staging_memory, 0, image_size, 0, &data),
             "map staging memory");
    std::memcpy(data, pixels, image_size);
    vkUnmapMemory(device_, staging_memory);

    // Image
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
             "create image");

    vkGetImageMemoryRequirements(device_, entry.image, &mem_req);
    alloc_info.allocationSize = mem_req.size;
    alloc_info.memoryTypeIndex = find_memory_type(
        physical_device_, mem_req.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vk_check(vkAllocateMemory(device_, &alloc_info, nullptr, &entry.memory),
             "allocate image memory");
    vk_check(vkBindImageMemory(device_, entry.image, entry.memory, 0),
             "bind image memory");

    // Transition + copy
    VkCommandBufferAllocateInfo cmd_alloc{};
    cmd_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_alloc.commandPool = command_pool_;
    cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_alloc.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vk_check(vkAllocateCommandBuffers(device_, &cmd_alloc, &cmd), "allocate cmd");

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(cmd, &begin), "begin cmd");

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
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
    vkCmdCopyBufferToImage(cmd, staging_buffer, entry.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    vk_check(vkEndCommandBuffer(cmd), "end cmd");

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vk_check(vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE), "submit cmd");
    vk_check(vkQueueWaitIdle(queue_), "wait upload");

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
             "create image view");

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
             "create sampler");

    entry.descriptor = ImGui_ImplVulkan_AddTexture(
        entry.sampler, entry.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    return entry;
}

// ── Icon loading ────────────────────────────────────────────────────────

void IconCache::load_icon(const std::string& name, const std::string& path) {
    int w, h, channels;
    auto* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!pixels) {
        FJELL_CORE_WARN("Failed to load icon: {}", path);
        return;
    }

    icons_[name] = upload_rgba(reinterpret_cast<const uint8_t*>(pixels), w, h);
    stbi_image_free(pixels);
}

// ── Thumbnails ──────────────────────────────────────────────────────────

VkDescriptorSet IconCache::thumbnail(const std::string& path) {
    auto it = thumbnails_.find(path);
    if (it != thumbnails_.end()) return it->second.descriptor;

    // Load and resize to thumbnail
    int w, h, channels;
    auto* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!pixels) return VK_NULL_HANDLE;

    // Downscale to 80x80 with simple box filter if larger
    constexpr int thumb_size = 80;
    std::vector<uint8_t> resized;
    const uint8_t* upload_data = reinterpret_cast<const uint8_t*>(pixels);
    int upload_w = w;
    int upload_h = h;

    if (w > thumb_size || h > thumb_size) {
        float scale = static_cast<float>(thumb_size) / static_cast<float>(std::max(w, h));
        upload_w = std::max(1, static_cast<int>(w * scale));
        upload_h = std::max(1, static_cast<int>(h * scale));
        resized.resize(upload_w * upload_h * 4);

        for (int dy = 0; dy < upload_h; ++dy) {
            for (int dx = 0; dx < upload_w; ++dx) {
                int sx = dx * w / upload_w;
                int sy = dy * h / upload_h;
                auto* src = reinterpret_cast<const uint8_t*>(pixels) + (sy * w + sx) * 4;
                auto* dst = resized.data() + (dy * upload_w + dx) * 4;
                dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3];
            }
        }
        upload_data = resized.data();
    }

    auto entry = upload_rgba(upload_data, upload_w, upload_h);
    stbi_image_free(pixels);

    thumbnails_[path] = entry;
    return entry.descriptor;
}

VkDescriptorSet IconCache::thumbnail_cached(const std::string& path) const {
    auto it = thumbnails_.find(path);
    return it != thumbnails_.end() ? it->second.descriptor : VK_NULL_HANDLE;
}

void IconCache::clear_thumbnails() {
    vkDeviceWaitIdle(device_);
    for (auto& [path, e] : thumbnails_) destroy_entry(device_, e);
    thumbnails_.clear();
}

void IconCache::preload_thumbnails(const std::vector<std::string>& paths, ThreadPool& pool) {
    namespace fs = std::filesystem;
    constexpr int THUMB_SIZE = 80;

    for (const auto& path : paths) {
        if (thumbnails_.contains(path)) continue;
        (void)pool.submit([this, path]() {
            namespace fs = std::filesystem;

            // Check for cached thumbnail on disk
            auto src_path = fs::path(path);
            auto cache_dir = src_path.parent_path() / ".fjcache";
            auto cache_path = cache_dir / (src_path.stem().string() + ".thumb.png");

            // Use cached file if it exists and is newer than the source
            bool cache_valid = false;
            if (fs::exists(cache_path) && fs::exists(src_path)) {
                auto src_time = fs::last_write_time(src_path);
                auto cache_time = fs::last_write_time(cache_path);
                cache_valid = (cache_time >= src_time);
            }

            int tw, th;
            std::vector<uint8_t> data;

            if (cache_valid) {
                // Load tiny cached thumbnail
                int channels;
                auto* pixels = stbi_load(cache_path.string().c_str(), &tw, &th, &channels, 4);
                if (!pixels) return;
                data.assign(pixels, pixels + static_cast<size_t>(tw) * th * 4);
                stbi_image_free(pixels);
            } else {
                // Decode full texture and downscale
                int w, h, channels;
                auto* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);
                if (!pixels) return;

                float scale = static_cast<float>(THUMB_SIZE) /
                              static_cast<float>(std::max(w, h));
                tw = std::max(1, static_cast<int>(w * scale));
                th = std::max(1, static_cast<int>(h * scale));

                if (w > THUMB_SIZE || h > THUMB_SIZE) {
                    data.resize(static_cast<size_t>(tw) * th * 4);
                    stbir_resize_uint8_linear(
                        pixels, w, h, 0,
                        data.data(), tw, th, 0,
                        STBIR_RGBA);
                } else {
                    tw = w; th = h;
                    data.assign(pixels, pixels + static_cast<size_t>(w) * h * 4);
                }
                stbi_image_free(pixels);

                // Write cache to disk
                std::error_code ec;
                fs::create_directories(cache_dir, ec);
                if (!ec) {
                    stbi_write_png(cache_path.string().c_str(), tw, th, 4,
                                   data.data(), tw * 4);
                }
            }

            std::lock_guard lock(pending_mutex_);
            pending_thumbnails_.push_back({path, std::move(data), tw, th});
        });
    }
}

void IconCache::poll_thumbnails() {
    constexpr int MAX_BATCH = 128;

    std::vector<PendingThumbnail> batch;
    {
        std::lock_guard lock(pending_mutex_);
        if (pending_thumbnails_.empty()) return;

        int count = std::min(static_cast<int>(pending_thumbnails_.size()), MAX_BATCH);
        batch.assign(
            std::make_move_iterator(pending_thumbnails_.begin()),
            std::make_move_iterator(pending_thumbnails_.begin() + count));
        pending_thumbnails_.erase(pending_thumbnails_.begin(),
                                  pending_thumbnails_.begin() + count);
    }

    // Filter out already-loaded
    std::erase_if(batch, [this](const PendingThumbnail& pt) {
        return thumbnails_.contains(pt.path);
    });
    if (batch.empty()) return;

    // Compute total staging size with alignment
    constexpr VkDeviceSize ALIGN = 64; // conservative alignment
    VkDeviceSize total_staging = 0;
    struct UploadInfo {
        VkDeviceSize staging_offset;
        int width, height;
    };
    std::vector<UploadInfo> infos(batch.size());

    for (size_t i = 0; i < batch.size(); ++i) {
        VkDeviceSize align_offset = (total_staging + ALIGN - 1) & ~(ALIGN - 1);
        infos[i].staging_offset = align_offset;
        infos[i].width = batch[i].width;
        infos[i].height = batch[i].height;
        total_staging = align_offset + static_cast<VkDeviceSize>(batch[i].width) * batch[i].height * 4;
    }

    // One staging buffer for all thumbnails
    VkBuffer staging_buffer;
    VkDeviceMemory staging_memory;

    VkBufferCreateInfo buf_info{};
    buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_info.size = total_staging;
    buf_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    vk_check(vkCreateBuffer(device_, &buf_info, nullptr, &staging_buffer),
             "create batch staging buffer");

    VkMemoryRequirements mem_req;
    vkGetBufferMemoryRequirements(device_, staging_buffer, &mem_req);

    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_req.size;
    alloc_info.memoryTypeIndex = find_memory_type(
        physical_device_, mem_req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vk_check(vkAllocateMemory(device_, &alloc_info, nullptr, &staging_memory),
             "allocate batch staging memory");
    vk_check(vkBindBufferMemory(device_, staging_buffer, staging_memory, 0),
             "bind batch staging memory");

    // Map and copy all pixel data
    uint8_t* mapped = nullptr;
    vk_check(vkMapMemory(device_, staging_memory, 0, total_staging, 0,
                          reinterpret_cast<void**>(&mapped)),
             "map batch staging memory");
    for (size_t i = 0; i < batch.size(); ++i) {
        VkDeviceSize img_size = static_cast<VkDeviceSize>(batch[i].width) * batch[i].height * 4;
        std::memcpy(mapped + infos[i].staging_offset, batch[i].pixels.data(), img_size);
    }
    vkUnmapMemory(device_, staging_memory);

    // Create all images + allocate memory
    std::vector<IconEntry> entries(batch.size());
    for (size_t i = 0; i < batch.size(); ++i) {
        VkImageCreateInfo img_info{};
        img_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        img_info.imageType = VK_IMAGE_TYPE_2D;
        img_info.extent = {static_cast<uint32_t>(infos[i].width),
                           static_cast<uint32_t>(infos[i].height), 1};
        img_info.mipLevels = 1;
        img_info.arrayLayers = 1;
        img_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        img_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        img_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        img_info.samples = VK_SAMPLE_COUNT_1_BIT;
        vk_check(vkCreateImage(device_, &img_info, nullptr, &entries[i].image),
                 "create batch image");

        vkGetImageMemoryRequirements(device_, entries[i].image, &mem_req);
        alloc_info.allocationSize = mem_req.size;
        alloc_info.memoryTypeIndex = find_memory_type(
            physical_device_, mem_req.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vk_check(vkAllocateMemory(device_, &alloc_info, nullptr, &entries[i].memory),
                 "allocate batch image memory");
        vk_check(vkBindImageMemory(device_, entries[i].image, entries[i].memory, 0),
                 "bind batch image memory");
    }

    // One command buffer: transition all → copy all → transition all
    VkCommandBufferAllocateInfo cmd_alloc{};
    cmd_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_alloc.commandPool = command_pool_;
    cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_alloc.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vk_check(vkAllocateCommandBuffers(device_, &cmd_alloc, &cmd), "allocate batch cmd");

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(cmd, &begin), "begin batch cmd");

    // Transition all to transfer dst
    for (size_t i = 0; i < entries.size(); ++i) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = entries[i].image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    // Copy all
    for (size_t i = 0; i < entries.size(); ++i) {
        VkBufferImageCopy region{};
        region.bufferOffset = infos[i].staging_offset;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {static_cast<uint32_t>(infos[i].width),
                              static_cast<uint32_t>(infos[i].height), 1};
        vkCmdCopyBufferToImage(cmd, staging_buffer, entries[i].image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }

    // Transition all to shader read
    for (size_t i = 0; i < entries.size(); ++i) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = entries[i].image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    vk_check(vkEndCommandBuffer(cmd), "end batch cmd");

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vk_check(vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE), "submit batch cmd");
    vk_check(vkQueueWaitIdle(queue_), "wait batch upload");

    vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
    vkDestroyBuffer(device_, staging_buffer, nullptr);
    vkFreeMemory(device_, staging_memory, nullptr);

    // Create views, samplers, descriptors
    for (size_t i = 0; i < entries.size(); ++i) {
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = entries[i].image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vk_check(vkCreateImageView(device_, &view_info, nullptr, &entries[i].view),
                 "create batch image view");

        VkSamplerCreateInfo sampler_info{};
        sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_info.magFilter = VK_FILTER_LINEAR;
        sampler_info.minFilter = VK_FILTER_LINEAR;
        sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vk_check(vkCreateSampler(device_, &sampler_info, nullptr, &entries[i].sampler),
                 "create batch sampler");

        entries[i].descriptor = ImGui_ImplVulkan_AddTexture(
            entries[i].sampler, entries[i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        thumbnails_[batch[i].path] = entries[i];
    }
}

} // namespace fjell
