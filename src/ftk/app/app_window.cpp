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

namespace {

// The window's root, to the UI core.
constexpr ui::core::RootId ROOT = 1;

// One channel of light as the sRGB value a display shows it by.
float encode_srgb(float linear) {
    const float c = std::clamp(linear, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

} // namespace

Result<std::unique_ptr<AppWindow>> AppWindow::create(const AppWindowDesc& desc) {
    // The window throws when the platform refuses it.
    try {
        auto window = std::make_unique<Window>(desc.title, desc.width, desc.height);
        auto device = gpu::Device::create(*window, desc.device);
        if (!device) return make_error(device.error());
        auto swapchain = gpu::Swapchain::create(**device, *window);
        if (!swapchain) return make_error(swapchain.error());
        auto ui = gpu::UiRenderer::create(**device, (*swapchain)->format());
        if (!ui) return make_error(ui.error());
        return std::unique_ptr<AppWindow>(new AppWindow(std::move(window), std::move(*device), std::move(*swapchain),
                                                        std::move(*ui), desc.clear));
    } catch (const std::exception& e) {
        return make_error(e.what());
    }
}

AppWindow::AppWindow(std::unique_ptr<Window> window, std::unique_ptr<gpu::Device> device,
                     std::unique_ptr<gpu::Swapchain> swapchain, std::unique_ptr<gpu::UiRenderer> ui_renderer,
                     gpu::Clear clear)
    : window_(std::move(window)), device_(std::move(device)), swapchain_(std::move(swapchain)),
      ui_renderer_(std::move(ui_renderer)), clear_(clear) {}

AppWindow::~AppWindow() {
    device_->wait_idle();
}

Result<> AppWindow::run(const AppFrame& frame) {
    UiInput input(*window_, ui_, ROOT);
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
        if (last_ui_ == nullptr) continue;
        for (const ui::core::RootOutput& root : last_ui_->roots) {
            if (root.root == ROOT) input.apply(root);
            for (Extra& extra : extras_) {
                if (extra.root == root.root) extra.input->apply(root);
            }
        }
        if (auto kept = keep_windows(*last_ui_); !kept) {
            device_->wait_idle();
            return kept;
        }
        // Asleep until there is input, or until what the frame waits on.
        const ui::core::NextFrame& next = last_ui_->next;
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

ui::Color AppWindow::background() const {
    return {encode_srgb(clear_.color[0]), encode_srgb(clear_.color[1]), encode_srgb(clear_.color[2]), 1.0f};
}

const ui::core::FrameOutput& AppWindow::draw_ui(const UiDraw& ui, glm::uvec2 pixels, float scale) {
    const glm::vec2 size = glm::vec2(pixels) / scale;
    ui::core::set_current(&ui_);
    // On the clock the platform stamps its events with.
    ui_.begin_frame(UiInput::now());
    ui_.begin_root(ROOT, size, scale);
    if (ui) ui(ui_, size);
    ui_.end_root();
    // The other windows: what is drawn into them was drawn among the main
    // window's, and their frames go on and end here.
    for (const Extra& extra : extras_) {
        const float extra_scale = extra.window->display_scale();
        const glm::vec2 extra_pixels{static_cast<float>(extra.swapchain->width()),
                                     static_cast<float>(extra.swapchain->height())};
        ui_.begin_root(extra.root, extra_pixels / extra_scale, extra_scale);
        ui_.end_root();
    }
    return ui_.end_frame();
}

Result<> AppWindow::draw_frame(const AppFrame& frame) {
    // At the display's scale: the window's pixels to a layout unit.
    const glm::uvec2 pixels{swapchain_->width(), swapchain_->height()};
    const ui::core::FrameOutput& out = draw_ui(frame.ui, pixels, window_->display_scale());
    last_ui_ = &out;
    if (auto shown = show(*swapchain_, ROOT, out); !shown) return shown;
    for (Extra& extra : extras_) {
        if (auto shown = show(*extra.swapchain, extra.root, out); !shown) return shown;
    }
    return {};
}

Result<> AppWindow::show(gpu::Swapchain& swapchain, ui::core::RootId root, const ui::core::FrameOutput& out) {
    const auto drawn = std::ranges::find(out.roots, root, &ui::core::RootOutput::root);
    if (drawn == out.roots.end()) return {};
    gpu::Frame& gpu_frame = device_->begin_frame();
    const auto image = swapchain.acquire(gpu_frame);
    if (!image) {
        (void)device_->end_frame(gpu_frame);
        return {};
    }
    gpu::CommandList& cmd = gpu_frame.commands(gpu::Queue::graphics);
    const gpu::Transition to_draw = image->to_draw();
    cmd.transition({&to_draw, 1});
    const glm::uvec2 pixels{swapchain.width(), swapchain.height()};
    ui_renderer_->render(cmd, image->texture, pixels, *drawn, ui_.text().atlas(), background());
    const gpu::Transition to_present = image->to_present();
    cmd.transition({&to_present, 1});
    gpu_frame.submit(cmd);
    return swapchain.present(gpu_frame, *image);
}

Result<> AppWindow::keep_windows(const ui::core::FrameOutput& out) {
    const auto close = [&](std::vector<Extra>::iterator extra) {
        // Its images may still be on the GPU.
        device_->wait_idle();
        extras_.erase(extra);
    };
    // Closed by their user: the UI hears of it.
    for (auto extra = extras_.begin(); extra != extras_.end();) {
        if (!extra->window->should_close()) {
            ++extra;
            continue;
        }
        ui_.closed_root(extra->root);
        close(extra);
        extra = extras_.begin();
    }
    for (const ui::core::RootId root : out.close) {
        const auto extra = std::ranges::find(extras_, root, &Extra::root);
        if (extra == extras_.end()) continue;
        close(extra);
        ui_.forget_root(root);
    }
    for (const ui::core::RootRequest& request : out.open) {
        if (std::ranges::contains(extras_, request.root, &Extra::root)) continue;
        // In points, as the platform sizes and places windows; beside the
        // window it was asked near, as near as the platform lets it.
        const Window* near = window_.get();
        float scale = window_->display_scale();
        float density = window_->pixel_density();
        for (const Extra& extra : extras_) {
            if (extra.root != request.near) continue;
            near = extra.window.get();
            scale = extra.window->display_scale();
            density = extra.window->pixel_density();
        }
        const float points = scale / density;
        try {
            auto window = std::make_unique<Window>(request.title,
                                                   static_cast<uint32_t>(std::max(request.size.x * points, 1.0f)),
                                                   static_cast<uint32_t>(std::max(request.size.y * points, 1.0f)),
                                                   *window_);
            window->set_position(near->position() + glm::ivec2(request.at * points));
            auto swapchain = gpu::Swapchain::create(*device_, *window);
            if (!swapchain) return make_error(swapchain.error());
            auto input = std::make_unique<UiInput>(*window, ui_, request.root);
            extras_.push_back({.root = request.root,
                               .window = std::move(window),
                               .swapchain = std::move(*swapchain),
                               .input = std::move(input)});
        } catch (const std::exception& e) {
            return make_error(e.what());
        }
        ui_.request_frame();
    }
    return {};
}

Result<Shot> AppWindow::shot(const UiDraw& ui, glm::uvec2 size, float scale, int frames) {
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
        const ui::core::FrameOutput& out = draw_ui(ui, size, scale);
        ui_renderer_->render(cmd, target.get(), size, out.roots.front(), ui_.text().atlas(), background());
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
