#include "ftk/app/standalone_window.hpp"

#include "ftk/app/ui_layer.hpp"
#include "ftk/base/log.hpp"
#include "ftk/gpu/command_list.hpp"
#include "ftk/gpu/device.hpp"
#include "ftk/gpu/frame.hpp"
#include "ftk/gpu/swapchain.hpp"
#include "ftk/gpu/window.hpp"

#include <stdexcept>

namespace ftk {

StandaloneWindow::StandaloneWindow(gpu::Device& device, const std::string& title, int width, int height,
                                   const UiPrepare& prepare)
    : device_{device} {
    window_ = std::make_unique<Window>(title, width, height);
    auto made = gpu::Swapchain::create(device_, *window_);
    if (!made) throw std::runtime_error(made.error());
    swapchain_ = std::move(*made);
    layer_ = std::make_unique<UiLayer>(*window_, device_, swapchain_->format());
    if (prepare) prepare(layer_->context());
}

StandaloneWindow::~StandaloneWindow() {
    // Its images and its UI's may still be on the GPU.
    device_.wait_idle();
    layer_.reset();
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
    const glm::uvec2 pixels = window_->framebuffer_size();
    if (pixels.x == 0 || pixels.y == 0) return;
    // The editor's context comes back after this window's frame.
    ui::core::Context* previous = ui::core::has_current() ? &ui::core::current() : nullptr;
    layer_->begin_frame();
    draw(layer_->size().x, layer_->size().y);
    (void)layer_->end_frame();
    ui::core::set_current(previous);

    gpu::Frame& frame = device_.begin_frame();
    const auto image = swapchain_->acquire(frame);
    if (!image) {
        (void)device_.end_frame(frame);
        return;
    }
    gpu::CommandList& cmd = frame.commands(gpu::Queue::graphics);
    const gpu::Transition to_draw = image->to_draw();
    cmd.transition({&to_draw, 1});
    layer_->render(cmd, image->texture, {swapchain_->width(), swapchain_->height()});
    const gpu::Transition to_present = image->to_present();
    cmd.transition({&to_present, 1});
    frame.submit(cmd);
    if (auto shown = swapchain_->present(frame, *image); !shown) {
        FTK_GFX_ERROR("{}", shown.error());
    }
    // Its popups past its edge, in windows of their own.
    if (auto shown = layer_->show_windows(); !shown) {
        FTK_GFX_ERROR("{}", shown.error());
    }
}

} // namespace ftk
