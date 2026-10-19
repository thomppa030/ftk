#include "ftk/app/ui_layer.hpp"

#include "ftk/app/ui_input.hpp"
#include "ftk/gpu/command_list.hpp"
#include "ftk/gpu/frame.hpp"
#include "ftk/gpu/swapchain.hpp"
#include "ftk/gpu/window.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <stdexcept>
#include <utility>

namespace ftk {

namespace {

// Every layer, to find the one whose context is current.
std::vector<UiLayer*>& layers() {
    static std::vector<UiLayer*> all;
    return all;
}

// One channel of light as the sRGB value a display shows it by.
float encode_srgb(float linear) {
    const float c = std::clamp(linear, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

} // namespace

ui::Color UiLayer::background() const {
    return {encode_srgb(background_.color[0]), encode_srgb(background_.color[1]), encode_srgb(background_.color[2]),
            1.0f};
}

UiLayer::UiLayer(Window& window, gpu::Device& device, gpu::Format format)
    : window_(window), device_(device), format_(format) {
    auto renderer = gpu::UiRenderer::create(device, format);
    if (!renderer) throw std::runtime_error("The UI renderer: " + renderer.error());
    renderer_ = std::move(*renderer);
    input_ = std::make_unique<UiInput>(window, ui_, ROOT);
    // It opens the OS popup windows a popup past the window's edge asks for.
    ui_.set_popups_leave_window(true);
    layers().push_back(this);
}

UiLayer::~UiLayer() {
    std::erase(layers(), this);
    if (ui::core::has_current() && &ui::core::current() == &ui_) ui::core::set_current(nullptr);
}

UiLayer* UiLayer::current() {
    if (!ui::core::has_current()) return nullptr;
    const ui::core::Context* context = &ui::core::current();
    for (UiLayer* layer : layers()) {
        if (&layer->ui_ == context) return layer;
    }
    return nullptr;
}

void UiLayer::begin_frame() {
    const float scale = window_.display_scale();
    const glm::uvec2 pixels = glm::max(window_.framebuffer_size(), glm::uvec2(1));
    fixed_ = false;
    begin_root(glm::vec2(pixels) / scale, scale);
}

void UiLayer::begin_frame(glm::uvec2 pixels, float scale) {
    fixed_ = true;
    begin_root(glm::vec2(glm::max(pixels, glm::uvec2(1))) / scale, scale);
}

void UiLayer::begin_root(glm::vec2 size, float scale) {
    frame_start_ = UiInput::now();
    forget_released();
    ui::core::set_current(&ui_);
    // On the clock the platform stamps its events with.
    ui_.begin_frame(UiInput::now());
    ui_.begin_root(ROOT, size, scale);
    size_ = size;
}

const ui::core::FrameOutput& UiLayer::end_frame() {
    ui_.end_root();
    // The other windows: what is drawn into them was drawn among the main
    // window's, and their frames go on and end here.
    if (!fixed_) {
        for (const Extra& extra : extras_) {
            const float scale = extra.window->display_scale();
            const glm::vec2 pixels{static_cast<float>(extra.swapchain->width()),
                                   static_cast<float>(extra.swapchain->height())};
            ui_.begin_root(extra.root, pixels / scale, scale);
            ui_.end_root();
        }
    }
    last_ = &ui_.end_frame();
    if (!fixed_) {
        for (const ui::core::RootOutput& root : last_->roots) {
            if (root.root == ROOT) input_->apply(root);
            for (Extra& extra : extras_) {
                if (extra.root == root.root) extra.input->apply(root);
            }
        }
    }
    building_.primitives = 0;
    for (const ui::core::RootOutput& root : last_->roots) {
        if (root.draw != nullptr) building_.primitives += root.draw->primitives.size();
    }
    building_.cpu_seconds = UiInput::now() - frame_start_;
    return *last_;
}

void UiLayer::render(gpu::CommandList& cmd, const gpu::TextureView& target, glm::uvec2 size) {
    if (last_ == nullptr) return;
    const auto drawn = std::ranges::find(last_->roots, ROOT, &ui::core::RootOutput::root);
    if (drawn == last_->roots.end()) return;
    const double start = UiInput::now();
    renderer_->render(cmd, target, size, *drawn, ui_.text().atlas(), background());
    cost_ = building_;
    cost_.cpu_seconds += UiInput::now() - start;
}

Result<> UiLayer::show_windows() {
    if (last_ == nullptr) return {};
    for (Extra& extra : extras_) {
        if (auto shown = show(*extra.swapchain, extra.root, *last_); !shown) return shown;
    }
    return keep_windows(*last_);
}

Result<> UiLayer::show(gpu::Swapchain& swapchain, ui::core::RootId root, const ui::core::FrameOutput& out) {
    const auto drawn = std::ranges::find(out.roots, root, &ui::core::RootOutput::root);
    if (drawn == out.roots.end()) return {};
    gpu::Frame& frame = device_.begin_frame();
    const auto image = swapchain.acquire(frame);
    if (!image) {
        (void)device_.end_frame(frame);
        return {};
    }
    gpu::CommandList& cmd = frame.commands(gpu::Queue::graphics);
    const gpu::Transition to_draw = image->to_draw();
    cmd.transition({&to_draw, 1});
    const glm::uvec2 pixels{swapchain.width(), swapchain.height()};
    renderer_->render(cmd, image->texture, pixels, *drawn, ui_.text().atlas(), background());
    const gpu::Transition to_present = image->to_present();
    cmd.transition({&to_present, 1});
    frame.submit(cmd);
    return swapchain.present(frame, *image);
}

Result<> UiLayer::keep_windows(const ui::core::FrameOutput& out) {
    const auto close = [&](std::vector<Extra>::iterator extra) {
        // Its images may still be on the GPU.
        device_.wait_idle();
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
        // In points, as the platform sizes and places windows; beside the
        // window it was asked near, as near as the platform lets it.
        const Window* near = &window_;
        float scale = window_.display_scale();
        float density = window_.pixel_density();
        for (const Extra& extra : extras_) {
            if (extra.root != request.near) continue;
            near = extra.window.get();
            scale = extra.window->display_scale();
            density = extra.window->pixel_density();
        }
        const float points = scale / density;
        const glm::uvec2 size{static_cast<uint32_t>(std::max(request.size.x * points, 1.0f)),
                              static_cast<uint32_t>(std::max(request.size.y * points, 1.0f))};
        // A popup is placed against the window it came from, on every
        // platform; another window on the desktop, where the platform lets
        // it.
        const bool popup = request.kind != ui::core::RootKind::Window;
        const glm::ivec2 offset{request.at * points};
        const glm::ivec2 at = popup ? offset : near->position() + offset;

        // Open already: a popup asked again, moved or sized as it is now.
        if (const auto open = std::ranges::find(extras_, request.root, &Extra::root); open != extras_.end()) {
            if (open->asked.at != request.at) open->window->set_position(at);
            if (open->asked.size != request.size) open->window->set_size(size);
            open->asked = request;
            continue;
        }
        // A popup whose window the platform cannot open is drawn inside the
        // window it came from instead, and so is every popup after it.
        const auto cannot_open = [&](std::string why) {
            if (popup) ui_.set_popups_leave_window(false);
            return make_error(std::move(why));
        };
        try {
            auto window = popup ? std::make_unique<Window>(*near, at, size.x, size.y,
                                                           request.kind == ui::core::RootKind::Tooltip)
                                : std::make_unique<Window>(request.title, size.x, size.y, window_);
            if (!popup) window->set_position(at);
            auto swapchain = gpu::Swapchain::create(device_, *window);
            if (!swapchain) return cannot_open(swapchain.error());
            auto input = std::make_unique<UiInput>(*window, ui_, request.root);
            extras_.push_back({.root = request.root,
                               .asked = request,
                               .window = std::move(window),
                               .swapchain = std::move(*swapchain),
                               .input = std::move(input)});
        } catch (const std::exception& e) {
            return cannot_open(e.what());
        }
        ui_.request_frame();
    }
    return {};
}

ui::Texture UiLayer::texture(const gpu::TextureView& view, gpu::UiPicture picture) {
    // A texture that is not there shows nothing.
    if (device_.info(view.texture).format == gpu::Format::undefined) return {};
    std::vector<Kept>& kept = pictures_[view.texture.id];
    for (const Kept& k : kept) {
        if (k.view == view && k.picture.sampler == picture.sampler && k.picture.linear == picture.linear) {
            return k.texture;
        }
    }
    const ui::Texture made = renderer_->texture(view, picture);
    kept.push_back({.view = view, .picture = picture, .texture = made});
    return made;
}

void UiLayer::forget_released() {
    for (auto it = pictures_.begin(); it != pictures_.end();) {
        const std::vector<Kept>& kept = it->second;
        if (kept.empty() || device_.info(kept.front().view.texture).format != gpu::Format::undefined) {
            ++it;
            continue;
        }
        for (const Kept& k : kept) renderer_->forget(k.texture);
        it = pictures_.erase(it);
    }
}

ui::Texture ui_texture(const gpu::TextureView& view, gpu::Sampler sampler) {
    UiLayer* layer = UiLayer::current();
    return layer != nullptr ? layer->texture(view, {.sampler = sampler, .linear = true}) : ui::Texture{};
}

} // namespace ftk
