#include "ftk/app/app_window.hpp"

#include "ftk/gpu/command_list.hpp"
#include "ftk/gpu/frame.hpp"
#include "ftk/gpu/owned.hpp"
#include "ftk/gpu/readback.hpp"
#include "ftk/gpu/swapchain.hpp"
#include "ftk/gpu/window.hpp"

#include <algorithm>
#include <exception>
#include <optional>
#include <utility>

namespace ftk {

Result<std::unique_ptr<AppWindow>> AppWindow::create(const AppWindowDesc& desc) {
    // The window and the ImGui layer throw when the platform refuses them.
    try {
        auto window = std::make_unique<Window>(desc.title, desc.width, desc.height);
        auto device = gpu::Device::create(*window, desc.device);
        if (!device) return make_error(device.error());
        auto swapchain = gpu::Swapchain::create(**device, *window);
        if (!swapchain) return make_error(swapchain.error());
        auto imgui = std::make_unique<ImGuiLayer>(*window, **device, (*swapchain)->format(), desc.files);
        return std::unique_ptr<AppWindow>(new AppWindow(std::move(window), std::move(*device),
                                                        std::move(*swapchain), std::move(imgui),
                                                        desc.clear));
    } catch (const std::exception& e) {
        return make_error(e.what());
    }
}

AppWindow::AppWindow(std::unique_ptr<Window> window, std::unique_ptr<gpu::Device> device,
                     std::unique_ptr<gpu::Swapchain> swapchain, std::unique_ptr<ImGuiLayer> imgui,
                     gpu::Clear clear)
    : window_(std::move(window)), device_(std::move(device)), swapchain_(std::move(swapchain)),
      imgui_(std::move(imgui)), clear_(clear) {}

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
        if (auto shown = draw_frame(frame.draw); !shown) {
            device_->wait_idle();
            return shown;
        }
    }
    device_->wait_idle();
    return {};
}

Result<> AppWindow::draw_frame(const std::function<void(float, float)>& draw) {
    gpu::Frame& frame = device_->begin_frame();
    const auto image = swapchain_->acquire(frame);
    if (!image) {
        (void)device_->end_frame(frame);
        return {};
    }

    gpu::CommandList& cmd = frame.commands(gpu::Queue::graphics);
    const gpu::Transition to_draw = image->to_draw();
    cmd.transition({&to_draw, 1});
    {
        auto target = cmd.render(
            {.color = {{.view = image->texture, .load = gpu::Load::clear, .clear = clear_}}});
        imgui_->begin_frame();
        if (draw) {
            draw(static_cast<float>(swapchain_->width()), static_cast<float>(swapchain_->height()));
        }
        imgui_->end_frame();
        imgui_->render(target);
    }
    const gpu::Transition to_present = image->to_present();
    cmd.transition({&to_present, 1});
    frame.submit(cmd);
    return swapchain_->present(frame, *image);
}

Result<Shot> AppWindow::shot(const std::function<void(float, float)>& draw, glm::uvec2 size,
                             int frames) {
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
    imgui_->draw_fixed(size);

    const int count = std::max(frames, 1);
    std::optional<gpu::Readback> read;
    for (int i = 0; i < count; ++i) {
        gpu::Frame& frame = device_->begin_frame();
        gpu::CommandList& cmd = frame.commands(gpu::Queue::graphics);
        // Every frame clears the texture, so nothing it holds is kept. The
        // first finds it new; the others after the last frame left it to be
        // read.
        const gpu::Transition to_draw{.texture = target.get(),
                                      .wait_for = i == 0 ? gpu::AccessSet{}
                                                         : gpu::AccessSet{gpu::Access::sampled_fragment},
                                      .visible_to = gpu::Access::color_attachment,
                                      .to = gpu::Access::color_attachment,
                                      .name = "shot"};
        cmd.transition({&to_draw, 1});
        {
            auto pass = cmd.render(
                {.color = {{.view = target.get(), .load = gpu::Load::clear, .clear = clear_}}});
            imgui_->begin_frame();
            if (draw) draw(static_cast<float>(size.x), static_cast<float>(size.y));
            imgui_->end_frame();
            imgui_->render(pass);
        }
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
