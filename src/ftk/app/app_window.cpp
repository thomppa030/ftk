#include "ftk/app/app_window.hpp"

#include "ftk/app/ui_input.hpp"
#include "ftk/gpu/command_list.hpp"
#include "ftk/gpu/frame.hpp"
#include "ftk/gpu/owned.hpp"
#include "ftk/gpu/readback.hpp"
#include "ftk/gpu/swapchain.hpp"
#include "ftk/gpu/window.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <optional>
#include <utility>

namespace ftk {

Result<std::unique_ptr<AppWindow>> AppWindow::create(const AppWindowDesc& desc) {
    // The window throws when the platform refuses it, and the layer when its
    // renderer can't be made.
    try {
        auto window = std::make_unique<Window>(desc.title, desc.width, desc.height);
        auto device = gpu::Device::create(*window, desc.device);
        if (!device) return make_error(device.error());
        auto swapchain = gpu::Swapchain::create(**device, *window);
        if (!swapchain) return make_error(swapchain.error());
        return std::unique_ptr<AppWindow>(
            new AppWindow(std::move(window), std::move(*device), std::move(*swapchain), desc.clear));
    } catch (const std::exception& e) {
        return make_error(e.what());
    }
}

AppWindow::AppWindow(std::unique_ptr<Window> window, std::unique_ptr<gpu::Device> device,
                     std::unique_ptr<gpu::Swapchain> swapchain, gpu::Clear clear)
    : window_(std::move(window)), device_(std::move(device)), swapchain_(std::move(swapchain)),
      layer_(std::make_unique<UiLayer>(*window_, *device_, swapchain_->format())) {
    layer_->set_background(clear);
}

AppWindow::~AppWindow() {
    device_->wait_idle();
}

Result<> AppWindow::run(const AppFrame& frame) {
    while (!window_->should_close()) {
        const glm::uvec2 size = window_->framebuffer_size();
        if (size.x == 0 || size.y == 0) {
            // Minimised: nothing can be drawn until the window comes back.
            window_->wait_events();
            continue;
        }
        window_->poll_events();
        if (frame.update) frame.update();
        if (auto shown = draw_frame(frame); !shown) {
            device_->wait_idle();
            return shown;
        }
        if (auto kept = layer_->show_windows(); !kept) {
            device_->wait_idle();
            return kept;
        }
        // Asleep until there is input, or until what the frame waits on.
        const ui::core::NextFrame& next = layer_->last()->next;
        if (next.now) continue;
        if (next.at) {
            window_->wait_events_for(*next.at - UiInput::now());
        } else {
            window_->wait_events();
        }
    }
    device_->wait_idle();
    return {};
}

Result<> AppWindow::draw_frame(const AppFrame& frame) {
    layer_->begin_frame();
    if (frame.ui) frame.ui(layer_->context(), layer_->size());
    (void)layer_->end_frame();

    gpu::Frame& gpu_frame = device_->begin_frame();
    const auto image = swapchain_->acquire(gpu_frame);
    if (!image) {
        (void)device_->end_frame(gpu_frame);
        return {};
    }
    gpu::CommandList& cmd = gpu_frame.commands(gpu::Queue::graphics);
    if (frame.record) frame.record(cmd);
    const gpu::Transition to_draw = image->to_draw();
    cmd.transition({&to_draw, 1});
    layer_->render(cmd, image->texture, {swapchain_->width(), swapchain_->height()});
    const gpu::Transition to_present = image->to_present();
    cmd.transition({&to_present, 1});
    gpu_frame.submit(cmd);
    return swapchain_->present(gpu_frame, *image);
}

Result<Shot> AppWindow::shot(const UiDraw& ui, glm::uvec2 size, float scale, int frames) {
    return shot(AppFrame{.ui = ui}, size, scale, frames);
}

Result<Shot> AppWindow::shot(const AppFrame& app_frame, glm::uvec2 size, float scale, int frames) {
    if (size.x == 0 || size.y == 0) return make_error("A shot needs a size of at least one pixel");
    auto made = device_->create(gpu::TextureDesc{
        .format = swapchain_->format(),
        .width = size.x,
        .height = size.y,
        .use = gpu::TextureUse::color_target | gpu::TextureUse::sampled,
        .name = "shot",
    });
    if (!made) return make_error(made.error());
    const gpu::Owned<gpu::Texture> target = std::move(*made);

    const int count = std::max(frames, 1);
    std::optional<gpu::Readback> read;
    for (int i = 0; i < count; ++i) {
        gpu::Frame& frame = device_->begin_frame();
        gpu::CommandList& cmd = frame.commands(gpu::Queue::graphics);
        // The first frame finds the texture new; the others after the last
        // frame left it to be read. Each draws all of it.
        const gpu::Transition to_draw{.texture = target.get(),
                                      .wait_for = i == 0 ? gpu::AccessSet{}
                                                         : gpu::AccessSet{gpu::Access::sampled_fragment},
                                      .visible_to = gpu::Access::color_attachment,
                                      .to = gpu::Access::color_attachment,
                                      .name = "shot"};
        cmd.transition({&to_draw, 1});
        layer_->begin_frame(size, scale);
        if (app_frame.ui) app_frame.ui(layer_->context(), layer_->size());
        (void)layer_->end_frame();
        if (app_frame.record) app_frame.record(cmd);
        layer_->render(cmd, target.get(), size);
        const gpu::Transition to_read{.texture = target.get(),
                                      .wait_for = gpu::Access::color_attachment,
                                      .flush = gpu::Access::color_attachment,
                                      .visible_to = gpu::Access::sampled_fragment,
                                      .from = gpu::Access::color_attachment,
                                      .to = gpu::Access::sampled_fragment,
                                      .name = "shot"};
        cmd.transition({&to_read, 1});
        if (i == count - 1) {
            auto reading = cmd.read_back(target.get(), {.use = gpu::Access::sampled_fragment});
            if (!reading) {
                (void)device_->end_frame(frame);
                return make_error(reading.error());
            }
            read.emplace(std::move(*reading));
        }
        frame.submit(cmd);
        if (auto ended = device_->end_frame(frame); !ended) return make_error(ended.error());
    }

    read->wait();
    Shot shot{.width = read->width(), .height = read->height()};
    const auto pixels = read->pixels();
    shot.pixels.assign(pixels.begin(), pixels.end());
    return shot;
}

} // namespace ftk
