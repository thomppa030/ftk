#pragma once

#include "ftk/gpu/format.hpp"
#include "ftk/gpu/owned.hpp"
#include "ftk/gpu/sampler.hpp"
#include "ftk/gpu/texture.hpp"

#include <cstdint>
#include <string>

namespace ftk::gpu {
class Device;
} // namespace ftk::gpu

namespace fjell {

/// An image the materials, sprites and UI sample through the bindless
/// table: its texture and the sampler it is read with.
class Texture {
public:
    /// Loads an image file as sRGB colour with a full mip chain, repeating
    /// and anisotropic; `nearest` reads it as pixel art: one mip, nearest
    /// filtering, clamped to the edge.
    Texture(ftk::gpu::Device& gpu, const std::string& path, bool nearest = false);

    /// `pixels` of `format`, row after row. `mip_levels` 0 makes a full chain.
    Texture(ftk::gpu::Device& gpu, const uint8_t* pixels, uint32_t width, uint32_t height,
            ftk::gpu::Format format = ftk::gpu::Format::rgba8_srgb,
            uint32_t mip_levels = 0,
            bool clamp_to_edge = false,
            bool nearest = false);

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;
    Texture(Texture&&) = delete;
    Texture& operator=(Texture&&) = delete;

    [[nodiscard]] ftk::gpu::Texture texture() const { return texture_; }
    [[nodiscard]] ftk::gpu::Sampler sampler() const { return sampler_; }
    [[nodiscard]] uint32_t width() const { return width_; }
    [[nodiscard]] uint32_t height() const { return height_; }

private:
    void upload(ftk::gpu::Device& gpu, const void* pixels, uint32_t width, uint32_t height,
                ftk::gpu::Format format, uint32_t mip_levels, bool clamp_to_edge, bool nearest);

    ftk::gpu::Owned<ftk::gpu::Texture> texture_;
    ftk::gpu::Sampler sampler_;
    uint32_t width_{0};
    uint32_t height_{0};
};

} // namespace fjell
