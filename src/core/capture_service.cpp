#include "core/capture_service.hpp"

#include "core/log.hpp"
#include "core/project.hpp"
#include "renderer/gpu/image_readback.hpp"
#include "renderer/gpu/swapchain.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/post_process_resources.hpp"
#include "renderer/vulkan_context.hpp"
#include "ui/viewport_manager.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <stb_image_write.h>
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

ImageReadback::Queue readback_queue(VulkanContext& vk) {
    return {
        .device = vk.device(),
        .allocator = vk.allocator(),
        .command_pool = vk.command_pool(),
        .queue = vk.graphics_queue(),
    };
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
    // descriptor reflects the resized post output — resizing AFTER
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
        // Through the warmup too, so exposure settles on the frame taken.
        saved_ungraded_ = vk.post_pass().ungraded();
        if (request_.ungraded) vk.post_pass().set_ungraded(true);
    }

    // Pin panel size so this frame's record path uses capture-res aspect
    // ratio in build_render_requests.
    panel->size.x = static_cast<float>(request_.width);
    panel->size.y = static_cast<float>(request_.height);

    // Resize the offscreen + PP. PP::resize touches ImGui descriptors via
    // PostPass::Images::resize → destroy → invalidate_imgui_texture.
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
    case Mode::scene:
        do_scene_capture(vk, viewports);
        vk.post_pass().set_ungraded(saved_ungraded_);
        break;
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
    // that resolution and the post output now holds the result.
    auto* output = vk.default_pp().post();
    if (output == nullptr || output->image == VK_NULL_HANDLE) {
        FJELL_CORE_ERROR("Capture: post output unavailable");
        return;
    }

    uint32_t w = output->extent.width;
    uint32_t h = output->extent.height;

    // The post output sits in SHADER_READ_ONLY_OPTIMAL after the frame.
    ImageReadback readback(readback_queue(vk), output->image,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, w, h);

    auto path = next_output_path("scene");
    write_png(path, w, h, readback.pixels());

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

    std::vector<uint8_t> cropped(static_cast<size_t>(w) * h * 4);
    {
        ImageReadback readback(readback_queue(vk), swap_image,
                               VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                               swap_extent.width, swap_extent.height);
        const uint8_t* src = readback.pixels();
        for (int row = 0; row < h; ++row) {
            const uint8_t* src_row = src + (((static_cast<size_t>(y0 + row) * swap_extent.width) + x0) * 4);
            std::memcpy(cropped.data() + (static_cast<size_t>(row) * w * 4), src_row,
                        static_cast<size_t>(w) * 4);
        }
    }

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
