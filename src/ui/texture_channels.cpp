#include "ui/texture_channels.hpp"

#include <cstddef>

namespace fjell {

std::vector<uint8_t> show_channels(std::span<const uint8_t> rgba, ChannelMask mask) {
    std::vector<uint8_t> out(rgba.size());
    const bool shown[4] = {mask.r, mask.g, mask.b, mask.a};
    int alone = -1;
    if (mask.count() == 1) {
        for (int c = 0; c < 4; ++c) {
            if (shown[c]) alone = c;
        }
    }
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
        if (alone >= 0) {
            const uint8_t v = rgba[i + static_cast<std::size_t>(alone)];
            out[i] = out[i + 1] = out[i + 2] = v;
            out[i + 3] = 255;
            continue;
        }
        for (std::size_t c = 0; c < 3; ++c) out[i + c] = shown[c] ? rgba[i + c] : uint8_t{0};
        out[i + 3] = mask.a ? rgba[i + 3] : uint8_t{255};
    }
    return out;
}

std::string_view channels_name(int channels) {
    switch (channels) {
        case 1: return "Grey";
        case 2: return "Grey and alpha";
        case 3: return "RGB";
        default: return "RGBA";
    }
}

} // namespace fjell
