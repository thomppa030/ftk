#include "renderer/resources/texture.hpp"
#include "ftk/gpu/device.hpp"
#include "ftk/gpu/upload.hpp"
#include "ftk/base/log.hpp"

#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <stdexcept>

namespace fjell {

namespace {

// Sixteen samples, or as many as the device allows.
constexpr float MAX_ANISOTROPY = 16.0f;

} // namespace

Texture::Texture(gpu::Device& gpu, const std::string& path, bool nearest) {
    int width, height, channels;
    stbi_uc* pixels = stbi_load(path.c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (!pixels) {
        throw std::runtime_error("Failed to load texture: " + path);
    }

    // Pixel art: no mipmaps, nearest filter, clamp to edge
    uint32_t mip = nearest ? 1 : 0;
    bool clamp = nearest;
    upload(gpu, pixels, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
           gpu::Format::rgba8_srgb, mip, clamp, nearest);
    stbi_image_free(pixels);

    FJELL_GFX_DEBUG("Texture loaded from file: {}x{} nearest={}", width, height, nearest);
}

Texture::Texture(gpu::Device& gpu, const uint8_t* pixels, uint32_t width, uint32_t height,
                 gpu::Format format, uint32_t mip_levels, bool clamp_to_edge, bool nearest) {
    upload(gpu, pixels, width, height, format, mip_levels, clamp_to_edge, nearest);

    FJELL_GFX_DEBUG("Texture loaded from memory: {}x{}", width, height);
}

void Texture::upload(gpu::Device& gpu, const void* pixels, uint32_t width, uint32_t height,
                     gpu::Format format, uint32_t mip_levels_override, bool clamp_to_edge,
                     bool nearest) {
    const uint32_t texel = gpu::texel_size(format);
    if (texel == 0) {
        throw std::runtime_error("Texture: no texel size known for this format");
    }
    const size_t image_size = static_cast<size_t>(width) * height * texel;
    const uint32_t mip_levels = mip_levels_override > 0 ? mip_levels_override :
        static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1;

    auto made = gpu.create(gpu::TextureDesc{
        .format = format,
        .width = width,
        .height = height,
        .mips = mip_levels,
        .use = gpu::TextureUse::sampled,
    });
    if (!made) {
        throw std::runtime_error("Texture: " + made.error());
    }
    texture_ = std::move(*made);
    width_ = width;
    height_ = height;

    // Materials sample it in fragment shaders, and the GI passes trace
    // through the same bindless array from compute.
    gpu.upload().to_texture(
        gpu::mip(texture_, 0), {},
        std::span(static_cast<const std::byte*>(pixels), image_size),
        {.after = gpu::Access::sampled_fragment | gpu::Access::sampled_compute,
         .generate_mips = mip_levels > 1});

    const gpu::Filter filter = nearest ? gpu::Filter::nearest : gpu::Filter::linear;
    sampler_ = gpu.sampler({
        .filter = filter,
        .mip_filter = filter,
        .address = clamp_to_edge ? gpu::Address::clamp : gpu::Address::repeat,
        .max_anisotropy = (clamp_to_edge || nearest) ? 0.0f : MAX_ANISOTROPY,
        .border = gpu::Border::opaque_black,
    });
}

} // namespace fjell
