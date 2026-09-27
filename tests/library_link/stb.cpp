// Links fjell-stb alone and whole (tests/CMakeLists.txt), so it builds only if
// everything in the library finds what it needs in the library and its own
// dependencies. Running it writes a small image to memory, reads it back and
// scales it.

#include <stb_image.h>
#include <stb_image_resize2.h>
#include <stb_image_write.h>

#include <vector>

namespace {

void append(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<unsigned char>*>(context);
    const auto* bytes = static_cast<const unsigned char*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

} // namespace

int main() {
    const unsigned char pixels[2 * 2 * 4] = {
        255, 0, 0, 255,  0, 255, 0, 255,
        0, 0, 255, 255,  255, 255, 255, 255,
    };
    std::vector<unsigned char> png;
    const int written = stbi_write_png_to_func(append, &png, 2, 2, 4, pixels, 2 * 4);

    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* decoded = stbi_load_from_memory(png.data(), static_cast<int>(png.size()),
                                                   &width, &height, &channels, 4);

    unsigned char resized[4 * 4 * 4];
    const unsigned char* scaled =
        stbir_resize_uint8_linear(pixels, 2, 2, 0, resized, 4, 4, 0, STBIR_RGBA);

    const bool ran = written != 0 && decoded != nullptr && width == 2 && height == 2 &&
                     scaled != nullptr;
    stbi_image_free(decoded);
    return ran ? 0 : 1;
}
