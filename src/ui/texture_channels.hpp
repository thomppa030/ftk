#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// Which of a texture's channels the texture preview shows (sheet 33).

namespace fjell {

struct ChannelMask {
    bool r{true};
    bool g{true};
    bool b{true};
    bool a{true};

    [[nodiscard]] int count() const { return int{r} + int{g} + int{b} + int{a}; }
    bool operator==(const ChannelMask&) const = default;
};

/// The picture of `rgba` (8-bit, four bytes a pixel) with only the channels
/// in `mask`: one channel alone is shown in grey, opaque; otherwise a hidden
/// colour channel reads zero and a hidden alpha reads opaque. With no
/// channel in the mask the picture is black.
[[nodiscard]] std::vector<uint8_t> show_channels(std::span<const uint8_t> rgba, ChannelMask mask);

/// The channels a file holds, by stb_image's count: "Grey", "Grey and alpha",
/// "RGB" or "RGBA".
[[nodiscard]] std::string_view channels_name(int channels);

} // namespace fjell
