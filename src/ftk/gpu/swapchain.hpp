#pragma once

#include "ftk/base/result.hpp"
#include "ftk/gpu/format.hpp"
#include "ftk/gpu/texture.hpp"
#include "ftk/gpu/transition.hpp"

#include <cstdint>
#include <memory>
#include <optional>

namespace ftk {
class Window;
}

namespace ftk::gpu {

class Frame;

/// One of a swapchain's images, acquired for a frame to draw into.
struct SwapchainImage {
    Texture texture{};
    /// Which of the swapchain's images it is.
    uint32_t index{0};

    /// Readies the image to be drawn into as `Access::color_attachment`. It
    /// arrives in `Access::present`, holding nothing worth keeping.
    [[nodiscard]] Transition to_draw() const {
        return {.texture = texture,
                .wait_for = Access::present,
                .visible_to = Access::color_attachment,
                .to = Access::color_attachment,
                .name = "swapchain"};
    }

    /// Hands the image back, drawn, to be presented.
    [[nodiscard]] Transition to_present() const {
        return {.texture = texture,
                .wait_for = Access::color_attachment,
                .flush = Access::color_attachment,
                .visible_to = Access::present,
                .from = Access::color_attachment,
                .to = Access::present,
                .name = "swapchain"};
    }
};

/// What a window shows: a few images that frames draw into in turn, each
/// shown once its frame is done. It is remade, with new images, whenever the
/// window's size changes, so an image is only good for the frame it was
/// acquired for. A frame may draw several windows: the program's own, whose
/// image it waits for, and others, which it draws when they have an image
/// ready; ending the frame shows them all.
///
/// @code
/// auto& frame = device.begin_frame();
/// auto image = swapchain->acquire(frame);
/// if (!image) {
///     (void)device.end_frame(frame);
///     return;
/// }
/// auto& cmd = frame.commands(gpu::Queue::graphics);
/// const gpu::Transition draw = image->to_draw();
/// cmd.transition({&draw, 1});
/// ... render into image->texture ...
/// const gpu::Transition show = image->to_present();
/// cmd.transition({&show, 1});
/// frame.submit(cmd);
/// (void)device.end_frame(frame);                     // shows the image
/// @endcode
class Swapchain {
public:
    /// The backend's state, which the backend defines.
    struct Impl;

    /// A swapchain for `window`, which must outlive it, drawn by `device`.
    /// @return the swapchain, or why the window cannot be drawn to (the
    ///         message says what the platform refused)
    [[nodiscard]] static Result<std::unique_ptr<Swapchain>> create(Device& device,
                                                                   Window& window);

    Swapchain(Device& device, std::unique_ptr<Impl> impl);
    /// Waits for the GPU to finish with the images first.
    ~Swapchain();

    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;
    Swapchain(Swapchain&&) = delete;
    Swapchain& operator=(Swapchain&&) = delete;

    /// The image `frame` draws the window into, waiting until the window has
    /// one: the program's own window, which paces its frames. The frame's
    /// first graphics list waits for it where it writes colour; work before
    /// that stage runs meanwhile. `Device::end_frame` shows it, drawn and
    /// left in `Access::present`.
    /// @return the image, or nothing when the window cannot take one now
    ///         because its size changed: the swapchain is remade for the next
    ///         frame (waiting while the window is minimised), and this frame
    ///         ends through `Device::end_frame` without one.
    [[nodiscard]] std::optional<SwapchainImage> acquire(Frame& frame);

    /// The image `frame` draws the window into, if the window has one ready
    /// now: another of the program's windows, which never holds the frame.
    /// The frame's last graphics list waits for it where it writes colour,
    /// so it is drawn there. `Device::end_frame` shows it, drawn and left in
    /// `Access::present`.
    /// @return the image, or nothing when the window has none ready (its
    ///         screen is still showing the ones it has, it is hidden, it has
    ///         no size), and the frame goes on without it
    [[nodiscard]] std::optional<SwapchainImage> acquire_ready(Frame& frame);

    /// The images' format and size, until the swapchain is remade.
    [[nodiscard]] Format format() const;
    [[nodiscard]] uint32_t width() const;
    [[nodiscard]] uint32_t height() const;

    [[nodiscard]] Impl& impl() noexcept { return *impl_; }

private:
    /// The image, waiting for one unless `ready_only`.
    [[nodiscard]] std::optional<SwapchainImage> take_image(Frame& frame, bool ready_only);

    Device* device_;
    std::unique_ptr<Impl> impl_;
};

} // namespace ftk::gpu
