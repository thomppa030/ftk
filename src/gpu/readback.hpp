#pragma once

#include "gpu/access.hpp"
#include "gpu/upload.hpp"

#include <cstdint>
#include <memory>
#include <span>

namespace fjell::gpu {

/// What `Device::read_back` reads, and how.
struct ReadbackDesc {
    /// The box of the view's mip read; a width of 0 is the whole mip.
    TextureRegion region{};
    /// The size the box is scaled to on the way, linearly filtered; 0 is the
    /// box's own size. Shrinking by more than about half aliases: scale the
    /// rest on the CPU.
    uint32_t width{0};
    uint32_t height{0};
    /// What the texture is used as around the read, which the read waits for
    /// and leaves it in.
    AccessSet use{Access::sampled_fragment};
};

/// A texture's texels on their way to the CPU as 8-bit RGBA, sRGB-encoded,
/// row after row. From `Device::read_back`, it reads what the GPU was given
/// to do before, and is sent with the next frame's uploads, or at `wait()`;
/// dropping it waits for the GPU to finish with it. From
/// `CommandList::read_back`, it reads what the frame's list recorded before
/// it and is ready once that frame has finished; dropping it waits for
/// nothing, since what it holds goes once that frame is done.
///
/// @code
/// auto read = device.read_back(output, {.use = gpu::Access::sampled_fragment});
/// if (read) {
///     read->wait();
///     write_png(path, read->width(), read->height(), read->pixels().data());
/// }
/// @endcode
class Readback {
public:
    /// The backend's state, which the backend defines.
    struct Impl;

    explicit Readback(std::unique_ptr<Impl> impl);
    ~Readback();

    Readback(Readback&&) noexcept;
    Readback& operator=(Readback&&) noexcept;
    Readback(const Readback&) = delete;
    Readback& operator=(const Readback&) = delete;

    /// Whether the GPU has finished and `pixels()` holds the texels.
    [[nodiscard]] bool ready() const;

    /// Sends the read if it has not gone yet and waits until it is ready. A
    /// read in a frame's list waits for that frame, which has to have been
    /// sent: before then it is reported and nothing waits.
    void wait();

    /// `width() * height() * 4` bytes once ready; empty before.
    [[nodiscard]] std::span<const uint8_t> pixels() const;
    [[nodiscard]] uint32_t width() const;
    [[nodiscard]] uint32_t height() const;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace fjell::gpu
