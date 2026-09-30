#pragma once

#include "gpu/format.hpp"
#include "gpu/owned.hpp"
#include "gpu/sampler.hpp"
#include "gpu/texture.hpp"

#include <cstdint>
#include <string>

namespace fjell {

namespace gpu {
class Device;
}

/// An image the materials, sprites and UI sample through the bindless
/// table: its texture and the sampler it is read with.
class Texture {
public:
    /// Loads an image file as sRGB colour with a full mip chain, repeating
    /// and anisotropic; `nearest` reads it as pixel art: one mip, nearest
    /// filtering, clamped to the edge.
    Texture(gpu::Device& gpu, const std::string& path, bool nearest = false);

    /// `pixels` of `format`, row after row. `mip_levels` 0 makes a full chain.
    Texture(gpu::Device& gpu, const uint8_t* pixels, uint32_t width, uint32_t height,
            gpu::Format format = gpu::Format::rgba8_srgb,
            uint32_t mip_levels = 0,
            bool clamp_to_edge = false,
            bool nearest = false);

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;
    Texture(Texture&&) = delete;
    Texture& operator=(Texture&&) = delete;

    [[nodiscard]] gpu::Texture texture() const { return texture_; }
    [[nodiscard]] gpu::Sampler sampler() const { return sampler_; }
    [[nodiscard]] uint32_t width() const { return width_; }
    [[nodiscard]] uint32_t height() const { return height_; }

private:
    void upload(gpu::Device& gpu, const void* pixels, uint32_t width, uint32_t height,
                gpu::Format format, uint32_t mip_levels, bool clamp_to_edge, bool nearest);

    gpu::Owned<gpu::Texture> texture_;
    gpu::Sampler sampler_;
    uint32_t width_{0};
    uint32_t height_{0};
};

} // namespace fjell
