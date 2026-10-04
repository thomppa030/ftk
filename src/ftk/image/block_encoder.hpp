#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ftk::image {

/// The block-compressed formats an image is encoded to, each 4 × 4 texels a
/// block.
enum class BlockFormat : uint8_t {
    bc4,  ///< one channel, the red of the texels given; 8 bytes a block
    bc5,  ///< two channels, red and green; 16 bytes a block
    bc6h, ///< colour above 1, unsigned, no alpha; 16 bytes a block
    bc7,  ///< colour with alpha; 16 bytes a block
};

/// Bytes of one block of `format`.
[[nodiscard]] uint32_t block_bytes(BlockFormat format) noexcept;

/// Rows of blocks an image `height` texels tall has, a part block at the
/// bottom edge counting as a row.
[[nodiscard]] constexpr uint32_t block_rows(uint32_t height) noexcept { return (height + 3) / 4; }

/// Bytes the blocks of a `width` × `height` image take, row of blocks after
/// row of blocks, a part block at the right or bottom edge taking a whole one.
[[nodiscard]] uint64_t encoded_size(BlockFormat format, uint32_t width, uint32_t height) noexcept;

/// An image of 8-bit RGBA texels packed row after row.
struct Rgba8Image {
    std::span<const uint8_t> texels;
    uint32_t width{0};
    uint32_t height{0};
};

/// An image of float RGBA texels, linear, packed row after row.
struct RgbaFloatImage {
    std::span<const float> texels;
    uint32_t width{0};
    uint32_t height{0};
};

/// Encodes images to one block-compressed format at one quality. Made once and
/// shared: any number of threads may encode different rows with it at once.
/// A part block at an image's right or bottom edge repeats the image's last
/// column or row.
class BlockEncoder {
public:
    /// An encoder for `format` at `quality`, from 0 (fastest) to 1 (best).
    BlockEncoder(BlockFormat format, float quality);
    ~BlockEncoder();

    BlockEncoder(BlockEncoder&& other) noexcept;
    BlockEncoder& operator=(BlockEncoder&& other) noexcept;
    BlockEncoder(const BlockEncoder&) = delete;
    BlockEncoder& operator=(const BlockEncoder&) = delete;

    [[nodiscard]] BlockFormat format() const noexcept { return format_; }

    /// Encodes `rows` rows of blocks of `image`, from row `first_row`, into
    /// their place in `blocks`, which holds every block of the image
    /// (`encoded_size`). BC4 reads red, BC5 red and green, BC7 all four.
    /// @return false, encoding nothing, for a BC6H encoder, for rows past the
    ///         image or for spans of the wrong size
    [[nodiscard]] bool encode(const Rgba8Image& image, uint32_t first_row, uint32_t rows,
                              std::span<std::byte> blocks) const;

    /// The same for a BC6H encoder, from linear float RGBA; alpha is not
    /// kept, and what an unsigned half cannot hold (below 0, NaN) is 0.
    /// @return false, encoding nothing, for any other encoder, for rows past
    ///         the image or for spans of the wrong size
    [[nodiscard]] bool encode(const RgbaFloatImage& image, uint32_t first_row, uint32_t rows,
                              std::span<std::byte> blocks) const;

private:
    void release() noexcept;

    BlockFormat format_;
    void* options_{nullptr};
};

/// `blocks` of a `width` × `height` image of `format` decoded back to 8-bit
/// RGBA, for seeing what an encode kept: BC4 as (r, 0, 0, 255), BC5 as
/// (r, g, 0, 255). Empty for BC6H, or when `blocks` is not the image's size.
[[nodiscard]] std::vector<uint8_t> decode(BlockFormat format, std::span<const std::byte> blocks,
                                          uint32_t width, uint32_t height);

/// BC6H `blocks` of a `width` × `height` image decoded back to linear float
/// RGBA, alpha 1. Empty when `blocks` is not the image's size.
[[nodiscard]] std::vector<float> decode_hdr(std::span<const std::byte> blocks, uint32_t width,
                                            uint32_t height);

} // namespace ftk::image
