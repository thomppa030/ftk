#pragma once

#include "core/result.hpp"
#include "gpu/buffer.hpp"
#include "gpu/owned.hpp"
#include "gpu/sampler.hpp"
#include "gpu/texture.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace fjell::gpu {

/// The GPU: what makes buffers, textures and samplers and destroys them once
/// the GPU is done with them.
///
/// Creating and releasing belong to one thread, the one that submits frames;
/// any thread may look up what exists (views and samplers included).
///
/// The backend's state lives in `Impl`, which only the backend defines. The
/// Vulkan backend makes a device with `vulkan::create_device()`.
class Device {
public:
    struct Impl;

    explicit Device(std::unique_ptr<Impl> impl);
    ~Device();

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    Device(Device&&) = delete;
    Device& operator=(Device&&) = delete;

    /// A buffer as described.
    /// @return the buffer, or the reason it could not be made (the message
    ///         names it).
    [[nodiscard]] Result<Owned<Buffer>> create(const BufferDesc& desc);

    /// A texture as described, holding `desc.initial` before the next frame
    /// when one is given.
    /// @return the texture, or the reason it could not be made (the message
    ///         names it).
    [[nodiscard]] Result<Owned<Texture>> create(const TextureDesc& desc);

    /// The sampler for `desc`: the same handle every time for the same
    /// description, valid as long as the device.
    [[nodiscard]] Sampler sampler(const SamplerDesc& desc);

    /// The CPU's view of a buffer in `Memory::upload` or `Memory::readback`;
    /// empty for `Memory::gpu` and for a handle that finds no buffer.
    [[nodiscard]] std::span<std::byte> mapped(Buffer buffer) const;

    /// A buffer's size in bytes; 0 for a handle that finds no buffer.
    [[nodiscard]] uint64_t size(Buffer buffer) const;

    /// What a texture was made as; an empty `TextureInfo` (0 × 0, undefined
    /// format) for a handle that finds no texture.
    [[nodiscard]] const TextureInfo& info(Texture texture) const;

    /// The backend's state, for the backend's own code.
    [[nodiscard]] Impl& impl() noexcept { return *impl_; }
    [[nodiscard]] const Impl& impl() const noexcept { return *impl_; }

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace fjell::gpu
