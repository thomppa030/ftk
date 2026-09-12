#include "core/capture_service.hpp"

#include "core/log.hpp"
#include "core/project.hpp"
#include "renderer/gpu/swapchain.hpp"
#include "renderer/gpu/vk_utils.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/post_process_resources.hpp"
#include "renderer/vulkan_context.hpp"
#include "ui/viewport_manager.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <stb_image_write.h>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <vector>

namespace fjell {

namespace {

VkCommandBuffer alloc_one_shot(VulkanContext& vk) {
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = vk.command_pool();
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd{};
    vkAllocateCommandBuffers(vk.device(), &ai, &cmd);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}

void submit_one_shot(VulkanContext& vk, VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(vk.graphics_queue(), 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(vk.graphics_queue());
}

struct Staging {
    VkBuffer buffer{VK_NULL_HANDLE};
    VmaAllocation alloc{VK_NULL_HANDLE};
    void* mapped{nullptr};
    VkDeviceSize size{0};
};

Staging create_staging(VmaAllocator alloc, uint32_t w, uint32_t h) {
    Staging s{};
    s.size = static_cast<VkDeviceSize>(w) * h * 4;
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = s.size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
               VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    vmaCreateBuffer(alloc, &bi, &ai, &s.buffer, &s.alloc, &info);
    s.mapped = info.pMappedData;
    return s;
}

void destroy_staging(VmaAllocator alloc, Staging& s) {
    if (s.buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(alloc, s.buffer, s.alloc);
    }
    s = {};
}

struct BlitImage {
    VkImage image{VK_NULL_HANDLE};
    VmaAllocation alloc{VK_NULL_HANDLE};
};

BlitImage create_blit_target(VmaAllocator alloc, uint32_t w, uint32_t h, VkFormat fmt) {
    BlitImage b{};
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    vmaCreateImage(alloc, &ci, &ai, &b.image, &b.alloc, nullptr);
    return b;
}

// Blit src -> transient sRGB image (handles missing TRANSFER_SRC bits and
// HDR -> 8-bit) then copy to staging buffer. Source is restored to
// `src_layout` on exit.
Staging readback_via_blit(VulkanContext& vk, VkImage src_image,
                          VkImageLayout src_layout, uint32_t w, uint32_t h) {
    BlitImage blit = create_blit_target(vk.allocator(), w, h,
                                        VK_FORMAT_R8G8B8A8_SRGB);

    Staging s = create_staging(vk.allocator(), w, h);
    VkCommandBuffer cmd = alloc_one_shot(vk);

    vk_utils::transition_image(cmd, src_image,
        src_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);

    vk_utils::transition_image(cmd, blit.image,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
        VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

    VkImageBlit region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffsets[1] = {static_cast<int32_t>(w), static_cast<int32_t>(h), 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstOffsets[1] = {static_cast<int32_t>(w), static_cast<int32_t>(h), 1};
    vkCmdBlitImage(cmd, src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   blit.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &region, VK_FILTER_NEAREST);

    vk_utils::transition_image(cmd, src_image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, src_layout,
        VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT);

    vk_utils::transition_image(cmd, blit.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);

    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cmd, blit.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           s.buffer, 1, &copy);

    submit_one_shot(vk, cmd);
    vkFreeCommandBuffers(vk.device(), vk.command_pool(), 1, &cmd);
    vmaDestroyImage(vk.allocator(), blit.image, blit.alloc);
    return s;
}

void bgra_to_rgba(uint8_t* data, size_t pixel_count) {
    for (size_t i = 0; i < pixel_count; ++i) {
        std::swap(data[(i * 4) + 0], data[(i * 4) + 2]);
    }
}

bool write_png(const std::filesystem::path& path, uint32_t w, uint32_t h,
               const void* rgba_data) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    int row_stride = static_cast<int>(w) * 4;
    int ok = stbi_write_png(path.string().c_str(), static_cast<int>(w),
                            static_cast<int>(h), 4, rgba_data, row_stride);
    if (ok == 0) {
        FJELL_CORE_ERROR("Capture: failed to write PNG");
        return false;
    }
    FJELL_CORE_INFO("Capture: wrote {}", path.string());
    return true;
}

/// Thread-safe local time. The reentrant call is spelled differently on each
/// platform, and the two take their arguments in opposite orders.
std::tm local_time(std::time_t t) {
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

} // namespace

std::filesystem::path CaptureService::next_output_path(const std::string& tag) const {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    const std::tm tm = local_time(t);

    std::stringstream ss;
    ss << tag << "_"
       << std::put_time(&tm, "%Y%m%d_%H%M%S")
       << ".png";

    std::filesystem::path dir = (project_ != nullptr)
        ? (project_->root() / "Screenshots")
        : std::filesystem::current_path() / "Screenshots";
    return dir / ss.str();
}

void CaptureService::request(const Request& r) {
    if (r.width == 0 || r.height == 0) {
        FJELL_CORE_ERROR("Capture: zero dimensions");
        return;
    }
    request_ = r;
    state_ = State::armed;
}

void CaptureService::resize_for_capture(VulkanContext& vk,
                                        ViewportManager& viewports) {
    // Called BEFORE draw_ui. Resizes the editor viewport's offscreen + PP
    // to the capture resolution if a scene capture is active. Must run
    // before ImGui draws the editor viewport panel so the ImGui texture
    // descriptor reflects the resized tonemap output — resizing AFTER
    // draw_ui would leave ImGui referencing a freed descriptor.
    if (state_ != State::armed && state_ != State::warming
            && state_ != State::capture_ready) {
        return;
    }
    if (request_.mode != Mode::scene) { return; }

    auto* panel = viewports.active_panel();
    if (panel == nullptr) {
        if (state_ == State::armed) {
            FJELL_CORE_ERROR("Capture: no active editor viewport");
        }
        state_ = State::idle;
        return;
    }

    if (state_ == State::armed) {
        saved_panel_id_ = panel->id;
        saved_panel_size_x_ = panel->size.x;
        saved_panel_size_y_ = panel->size.y;
        warmup_remaining_ = kWarmupFrames;
        state_ = State::warming;
    }

    // Pin panel size so this frame's record path uses capture-res aspect
    // ratio in build_render_requests.
    panel->size.x = static_cast<float>(request_.width);
    panel->size.y = static_cast<float>(request_.height);

    // Resize the offscreen + PP. PP::resize touches ImGui descriptors via
    // TonemapPass::Images::resize → destroy → invalidate_imgui_texture.
    // wait_idle ensures no in-flight CB references them.
    if (panel->offscreen) {
        if (panel->offscreen->extent().width != request_.width
                || panel->offscreen->extent().height != request_.height) {
            vk.wait_idle();
            panel->offscreen->resize(request_.width, request_.height);
            vk.default_pp().resize(request_.width, request_.height);
        }
    }
}

void CaptureService::prepare_for_record(VulkanContext& vk,
                                        ViewportManager& /*viewports*/) {
    // Called AFTER draw_ui. Pane mode advances state here. Scene mode
    // already did its resize in resize_for_capture; nothing to do here
    // because draw_ui doesn't overwrite the offscreen sizes — only
    // panel.size, which we don't use again this frame.
    (void)vk;
    if (state_ == State::armed && request_.mode == Mode::pane) {
        state_ = State::capture_ready;
    }
}

void CaptureService::finish_after_present(VulkanContext& vk,
                                          ViewportManager& viewports) {
    if (state_ == State::warming) {
        if (--warmup_remaining_ <= 0) {
            state_ = State::capture_ready;
        }
        return;
    }

    if (state_ != State::capture_ready) { return; }

    switch (request_.mode) {
    case Mode::scene: do_scene_capture(vk, viewports); break;
    case Mode::pane:  do_pane_capture(vk); break;
    }

    state_ = State::idle;
}

void CaptureService::do_scene_capture(VulkanContext& vk,
                                      ViewportManager& viewports) {
    auto* panel = viewports.active_panel();
    if (panel == nullptr || !panel->offscreen) {
        FJELL_CORE_ERROR("Capture: panel/offscreen vanished mid-capture");
        return;
    }

    // The panel was resized to the capture dimensions; it just rendered at
    // that resolution and tonemap output now holds the result.
    auto* tonemap = vk.default_pp().tonemap();
    if (tonemap == nullptr || tonemap->image == VK_NULL_HANDLE) {
        FJELL_CORE_ERROR("Capture: tonemap output unavailable");
        return;
    }

    uint32_t w = tonemap->extent.width;
    uint32_t h = tonemap->extent.height;

    // Tonemap output sits in SHADER_READ_ONLY_OPTIMAL after the frame.
    Staging s = readback_via_blit(vk, tonemap->image,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                  w, h);

    auto path = next_output_path("scene");
    write_png(path, w, h, s.mapped);
    destroy_staging(vk.allocator(), s);

    // Restore the panel size so the next frame's apply_resize puts the
    // editor viewport back to its on-screen size.
    if (panel->id == saved_panel_id_) {
        panel->size.x = saved_panel_size_x_;
        panel->size.y = saved_panel_size_y_;
    }
}

void CaptureService::do_pane_capture(VulkanContext& vk) {
    ImGuiWindow* win = ImGui::FindWindowByName(request_.pane_name.c_str());
    if (win == nullptr) {
        FJELL_CORE_ERROR("Capture: pane not found");
        return;
    }

    VkExtent2D swap_extent = vk.swapchain_extent();
    int x0 = std::max(0, static_cast<int>(win->Pos.x));
    int y0 = std::max(0, static_cast<int>(win->Pos.y));
    int x1 = std::min(static_cast<int>(swap_extent.width),
                      static_cast<int>(win->Pos.x + win->Size.x));
    int y1 = std::min(static_cast<int>(swap_extent.height),
                      static_cast<int>(win->Pos.y + win->Size.y));
    int w = x1 - x0;
    int h = y1 - y0;
    if (w <= 0 || h <= 0) {
        FJELL_CORE_ERROR("Capture: pane has zero on-screen area");
        return;
    }

    vk.wait_idle();

    uint32_t image_index = vk.recorded_swapchain_image_index();
    VkImage swap_image = vk.gpu().swapchain().image(image_index);

    Staging s = readback_via_blit(vk, swap_image,
                                  VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                  swap_extent.width, swap_extent.height);

    std::vector<uint8_t> cropped(static_cast<size_t>(w) * h * 4);
    const uint8_t* src = static_cast<const uint8_t*>(s.mapped);
    for (int row = 0; row < h; ++row) {
        const uint8_t* src_row = src + (((static_cast<size_t>(y0 + row) * swap_extent.width) + x0) * 4);
        std::memcpy(cropped.data() + (static_cast<size_t>(row) * w * 4), src_row,
                    static_cast<size_t>(w) * 4);
    }
    destroy_staging(vk.allocator(), s);

    bgra_to_rgba(cropped.data(), static_cast<size_t>(w) * h);

    std::string tag = "pane_";
    for (char c : request_.pane_name) {
        tag += (std::isalnum(static_cast<unsigned char>(c)) != 0) ? c : '_';
        if (tag.size() > 48) { break; }
    }
    auto path = next_output_path(tag);
    write_png(path, static_cast<uint32_t>(w), static_cast<uint32_t>(h),
              cropped.data());
}

} // namespace fjell
