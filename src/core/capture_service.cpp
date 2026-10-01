#include "core/capture_service.hpp"

#include "core/project.hpp"
#include "ftk/base/log.hpp"
#include "ftk/gpu/swapchain.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/post_process_resources.hpp"
#include "renderer/vulkan_context.hpp"
#include "ui/viewport_manager.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <stb_image_write.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace fjell {

namespace {

bool write_png(const std::filesystem::path& path, uint32_t w, uint32_t h,
               const void* rgba_data) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    int row_stride = static_cast<int>(w) * 4;
    int ok = stbi_write_png(path.string().c_str(), static_cast<int>(w),
                            static_cast<int>(h), 4, rgba_data, row_stride);
    if (ok == 0) {
        FTK_CORE_ERROR("Capture: failed to write PNG");
        return false;
    }
    FTK_CORE_INFO("Capture: wrote {}", path.string());
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
        FTK_CORE_ERROR("Capture: zero dimensions");
        return;
    }
    request_ = r;
    state_ = State::armed;
}

void CaptureService::resize_for_capture(VulkanContext& vk,
                                        ViewportManager& viewports) {
    // Called BEFORE draw_ui. Resizes the editor viewport's offscreen + PP
    // to the capture resolution if a scene capture is active, so the
    // viewport panel drawn this frame already shows the resized post output.
    if (state_ != State::armed && state_ != State::warming
            && state_ != State::capture_ready) {
        return;
    }
    if (request_.mode != Mode::scene) { return; }

    auto* panel = viewports.active_panel();
    if (panel == nullptr) {
        if (state_ == State::armed) {
            FTK_CORE_ERROR("Capture: no active editor viewport");
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

    // Resize the offscreen + PP.
    if (panel->offscreen) {
        if (panel->offscreen->extent().x != request_.width
                || panel->offscreen->extent().y != request_.height) {
            panel->offscreen->resize(request_.width, request_.height);
            if (panel->pp) panel->pp->resize(request_.width, request_.height);
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

void CaptureService::record_read(VulkanContext& vk, ViewportManager& viewports) {
    if (state_ != State::capture_ready) return;
    switch (request_.mode) {
    case Mode::scene:
        read_scene(vk, viewports);
        vk.post_pass().set_ungraded(saved_ungraded_);
        break;
    case Mode::pane: read_pane(vk); break;
    }
    state_ = read_ ? State::reading : State::idle;
}

void CaptureService::finish_after_present(VulkanContext& /*vk*/, ViewportManager& /*viewports*/) {
    if (state_ == State::warming) {
        if (--warmup_remaining_ <= 0) {
            state_ = State::capture_ready;
        }
        return;
    }

    if (state_ != State::reading || !read_ || !read_->ready()) return;
    write_png(next_output_path(read_tag_), read_->width(), read_->height(), read_->pixels().data());
    read_.reset();
    state_ = State::idle;
}

void CaptureService::read_scene(VulkanContext& vk, ViewportManager& viewports) {
    auto* panel = viewports.active_panel();
    if (panel == nullptr || !panel->offscreen) {
        FTK_CORE_ERROR("Capture: panel/offscreen vanished mid-capture");
        return;
    }

    // The panel was resized to the capture dimensions, and this frame
    // renders it at that resolution into the post output.
    auto* output = panel->pp ? panel->pp->post() : nullptr;
    if (output == nullptr || !output->texture) {
        FTK_CORE_ERROR("Capture: post output unavailable");
        return;
    }

    // The editor's viewport samples the post output.
    auto readback = vk.read_back_in_frame(output->texture, {.use = ftk::gpu::Access::sampled_fragment});
    if (!readback) {
        FTK_CORE_ERROR("Capture: {}", readback.error());
        return;
    }
    read_ = std::move(*readback);
    read_tag_ = "scene";

    // Back to the panel's own size, so the next frame's apply_resize puts
    // the editor viewport back to its on-screen size. The read keeps the
    // image it reads until its frame is done.
    if (panel->id == saved_panel_id_) {
        panel->size.x = saved_panel_size_x_;
        panel->size.y = saved_panel_size_y_;
    }
}

void CaptureService::read_pane(VulkanContext& vk) {
    ImGuiWindow* win = ImGui::FindWindowByName(request_.pane_name.c_str());
    if (win == nullptr) {
        FTK_CORE_ERROR("Capture: pane not found");
        return;
    }

    const ftk::gpu::Swapchain& swapchain = vk.gpu().swapchain();
    int x0 = std::max(0, static_cast<int>(win->Pos.x));
    int y0 = std::max(0, static_cast<int>(win->Pos.y));
    int x1 = std::min(static_cast<int>(swapchain.width()),
                      static_cast<int>(win->Pos.x + win->Size.x));
    int y1 = std::min(static_cast<int>(swapchain.height()),
                      static_cast<int>(win->Pos.y + win->Size.y));
    int w = x1 - x0;
    int h = y1 - y0;
    if (w <= 0 || h <= 0) {
        FTK_CORE_ERROR("Capture: pane has zero on-screen area");
        return;
    }

    // This frame's own image, with the UI drawn and not yet presented. The
    // read converts to RGBA whatever order the swapchain keeps.
    auto readback = vk.read_back_in_frame(
        vk.frame_swapchain_texture(),
        {.region = {.x = static_cast<uint32_t>(x0),
                    .y = static_cast<uint32_t>(y0),
                    .width = static_cast<uint32_t>(w),
                    .height = static_cast<uint32_t>(h)},
         .use = ftk::gpu::Access::present});
    if (!readback) {
        FTK_CORE_ERROR("Capture: {}", readback.error());
        return;
    }
    read_ = std::move(*readback);

    read_tag_ = "pane_";
    for (char c : request_.pane_name) {
        read_tag_ += (std::isalnum(static_cast<unsigned char>(c)) != 0) ? c : '_';
        if (read_tag_.size() > 48) { break; }
    }
}

} // namespace fjell
