#include "ui/standalone_window.hpp"

#include "core/log.hpp"
#include "gpu/command_list.hpp"
#include "gpu/device.hpp"
#include "gpu/frame.hpp"
#include "gpu/swapchain.hpp"
#include "renderer/gpu/window.hpp"
#include "ui/imgui_layer.hpp"

#include <stdexcept>

namespace fjell {

namespace {

// Under the ImGui window, which covers all of it.
constexpr gpu::Clear CLEAR{0.012f, 0.012f, 0.015f, 1.0f};

} // namespace

StandaloneWindow::StandaloneWindow(gpu::Device& device, const std::string& title, int width, int height,
                                   const ImGuiLayerFiles& files)
    : device_{device} {
    window_ = std::make_unique<Window>(title, width, height);
    auto made = gpu::Swapchain::create(device_, *window_);
    if (!made) throw std::runtime_error(made.error());
    swapchain_ = std::move(*made);
    imgui_ = std::make_unique<ImGuiLayer>(*window_, device_, swapchain_->format(), files);
}

StandaloneWindow::~StandaloneWindow() {
    // ImGui destroys its vertex buffers at once, which the GPU may still be
    // drawing from.
    device_.wait_idle();
    imgui_.reset();
    swapchain_.reset();
    window_.reset();
}

void StandaloneWindow::request_close() {
    window_->request_close();
}

bool StandaloneWindow::close_requested() const {
    return window_->should_close();
}

void StandaloneWindow::frame(const std::function<void(float width, float height)>& draw) {
    gpu::Device& device = device_;
    gpu::Frame& frame = device.begin_frame();
    const auto image = swapchain_->acquire(frame);
    if (!image) {
        (void)device.end_frame(frame);
        return;
    }

    gpu::CommandList& cmd = frame.commands(gpu::Queue::graphics);
    const gpu::Transition to_draw = image->to_draw();
    cmd.transition({&to_draw, 1});
    {
        auto target = cmd.render({.color = {{.view = image->texture, .load = gpu::Load::clear, .clear = CLEAR}}});
        imgui_->activate();
        imgui_->begin_frame();
        draw(static_cast<float>(swapchain_->width()), static_cast<float>(swapchain_->height()));
        imgui_->end_frame();
        imgui_->render(target);
        imgui_->deactivate();
    }
    const gpu::Transition to_present = image->to_present();
    cmd.transition({&to_present, 1});
    frame.submit(cmd);
    if (auto shown = swapchain_->present(frame, *image); !shown) {
        FJELL_GFX_ERROR("{}", shown.error());
    }
}

} // namespace fjell
