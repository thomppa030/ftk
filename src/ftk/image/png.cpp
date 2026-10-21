#include "ftk/image/png.hpp"

#include <stb_image.h>
#include <stb_image_write.h>

#include <limits>

#include <cstring>

namespace ftk::image {

std::vector<std::byte> encode_png(std::span<const uint8_t> texels, uint32_t width, uint32_t height,
                                  uint32_t channels) {
    std::vector<std::byte> png;
    if (width == 0 || height == 0 || channels < 1 || channels > 4) return png;
    if (texels.size() != std::size_t{width} * height * channels) return png;
    // stb hands the file over in pieces as it writes it.
    const auto append = [](void* to, void* data, int size) {
        auto& bytes = *static_cast<std::vector<std::byte>*>(to);
        const std::size_t at = bytes.size();
        bytes.resize(at + static_cast<std::size_t>(size));
        std::memcpy(bytes.data() + at, data, static_cast<std::size_t>(size));
    };
    const int stride = static_cast<int>(width * channels);
    if (stbi_write_png_to_func(append, &png, static_cast<int>(width), static_cast<int>(height),
                               static_cast<int>(channels), texels.data(), stride) == 0) {
        png.clear();
    }
    return png;
}

std::optional<Rgba8> decode_png(std::span<const std::byte> bytes) {
    if (bytes.empty() || bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return std::nullopt;
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* texels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), static_cast<int>(bytes.size()),
                                            &width, &height, &channels, 4);
    if (texels == nullptr) return std::nullopt;
    Rgba8 image{.texels = {}, .width = static_cast<uint32_t>(width), .height = static_cast<uint32_t>(height)};
    image.texels.assign(texels, texels + static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    stbi_image_free(texels);
    return image;
}

} // namespace ftk::image
