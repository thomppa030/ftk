#include "ftk/image/png.hpp"

#include <stb_image_write.h>

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

} // namespace ftk::image
