#include "ftk/image/block_encoder.hpp"

// cmp_core.h names NULL without including what defines it.
#include <cstddef>
#include <cmp_core.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>

namespace ftk::image {

namespace {

constexpr uint32_t BLOCK = 4;

// CMP_Core fills BC7's tables the first time options are made for it, and
// marks them filled before it fills them: a second thread making options at
// that moment encodes with half-made tables. Making the first options under a
// static's initialisation, which runs on one thread while any other waits,
// leaves every later call finding them made.
void fill_bc7_tables() {
    static const bool filled = [] {
        void* options = nullptr;
        CreateOptionsBC7(&options);
        DestroyOptionsBC7(options);
        return true;
    }();
    (void)filled;
}

uint32_t blocks_across(uint32_t width) {
    return (width + BLOCK - 1) / BLOCK;
}

// The unsigned half nearest `value`, rounding to even, for BC6H: below 0 and
// NaN are 0, and what is past the largest half is the largest half.
uint16_t to_half(float value) {
    if (!(value > 0.0f)) return 0;
    constexpr float LARGEST = 65504.0f;
    if (value >= LARGEST) return 0x7BFF;
    const auto bits = std::bit_cast<uint32_t>(value);
    const int32_t exponent = static_cast<int32_t>((bits >> 23) & 0xFFU) - 127 + 15;
    uint32_t mantissa = bits & 0x7FFFFFU;
    if (exponent <= 0) {
        // Under the smallest normal half: a subnormal, or 0.
        if (exponent < -10) return 0;
        mantissa |= 0x800000U;
        const auto shift = static_cast<uint32_t>(14 - exponent);
        uint32_t half = mantissa >> shift;
        const uint32_t rest = mantissa & ((1U << shift) - 1U);
        const uint32_t halfway = 1U << (shift - 1U);
        if (rest > halfway || (rest == halfway && (half & 1U) != 0)) ++half;
        return static_cast<uint16_t>(half);
    }
    uint32_t half = (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13);
    const uint32_t rest = mantissa & 0x1FFFU;
    if (rest > 0x1000U || (rest == 0x1000U && (half & 1U) != 0)) ++half;
    return static_cast<uint16_t>(half);
}

float from_half(uint16_t half) {
    const uint32_t exponent = (half >> 10) & 0x1FU;
    const uint32_t mantissa = half & 0x3FFU;
    const float sign = (half & 0x8000U) != 0 ? -1.0f : 1.0f;
    if (exponent == 0) return sign * std::ldexp(static_cast<float>(mantissa), -24);
    if (exponent == 31) {
        return mantissa != 0 ? std::numeric_limits<float>::quiet_NaN()
                             : sign * std::numeric_limits<float>::infinity();
    }
    return sign * std::ldexp(static_cast<float>(mantissa | 0x400U), static_cast<int>(exponent) - 25);
}

// Whether `rows` rows of blocks from `first_row` lie in an image of
// `height`, and the image and its blocks have the sizes they should.
bool fits(BlockFormat format, uint32_t width, uint32_t height, size_t texels, uint32_t first_row,
          uint32_t rows, size_t blocks) {
    return width != 0 && height != 0 && texels == size_t{width} * height * 4 &&
           blocks == encoded_size(format, width, height) &&
           uint64_t{first_row} + rows <= block_rows(height);
}

unsigned char* block_at(std::span<std::byte> blocks, BlockFormat format, uint32_t width, uint32_t x,
                        uint32_t y) {
    const size_t index = size_t{y} * blocks_across(width) + x;
    return reinterpret_cast<unsigned char*>(blocks.data() + index * block_bytes(format));
}

const unsigned char* block_at(std::span<const std::byte> blocks, BlockFormat format, uint32_t width,
                              uint32_t x, uint32_t y) {
    const size_t index = size_t{y} * blocks_across(width) + x;
    return reinterpret_cast<const unsigned char*>(blocks.data() + index * block_bytes(format));
}

// The first of the four values of the texel a block's texel (i, j) reads:
// the image's own, or past its right or bottom edge, the last one there.
size_t source_of(uint32_t width, uint32_t height, uint32_t block_x, uint32_t block_y, uint32_t i,
                 uint32_t j) {
    const uint32_t x = std::min(block_x * BLOCK + i, width - 1);
    const uint32_t y = std::min(block_y * BLOCK + j, height - 1);
    return (size_t{y} * width + x) * 4;
}

} // namespace

uint32_t block_bytes(BlockFormat format) noexcept {
    return format == BlockFormat::bc4 ? 8 : 16;
}

uint64_t encoded_size(BlockFormat format, uint32_t width, uint32_t height) noexcept {
    return uint64_t{block_bytes(format)} * blocks_across(width) * block_rows(height);
}

BlockEncoder::BlockEncoder(BlockFormat format, float quality) : format_(format) {
    quality = std::clamp(quality, 0.0f, 1.0f);
    switch (format_) {
        case BlockFormat::bc4:
            CreateOptionsBC4(&options_);
            SetQualityBC4(options_, quality);
            break;
        case BlockFormat::bc5:
            CreateOptionsBC5(&options_);
            SetQualityBC5(options_, quality);
            break;
        case BlockFormat::bc6h:
            CreateOptionsBC6(&options_);
            SetQualityBC6(options_, quality);
            SetSignedBC6(options_, false);
            break;
        case BlockFormat::bc7:
            fill_bc7_tables();
            CreateOptionsBC7(&options_);
            SetQualityBC7(options_, quality);
            break;
    }
}

BlockEncoder::~BlockEncoder() {
    release();
}

BlockEncoder::BlockEncoder(BlockEncoder&& other) noexcept
    : format_(other.format_), options_(std::exchange(other.options_, nullptr)) {}

BlockEncoder& BlockEncoder::operator=(BlockEncoder&& other) noexcept {
    if (this != &other) {
        release();
        format_ = other.format_;
        options_ = std::exchange(other.options_, nullptr);
    }
    return *this;
}

void BlockEncoder::release() noexcept {
    if (options_ == nullptr) return;
    switch (format_) {
        case BlockFormat::bc4:  DestroyOptionsBC4(options_); break;
        case BlockFormat::bc5:  DestroyOptionsBC5(options_); break;
        case BlockFormat::bc6h: DestroyOptionsBC6(options_); break;
        case BlockFormat::bc7:  DestroyOptionsBC7(options_); break;
    }
    options_ = nullptr;
}

bool BlockEncoder::encode(const Rgba8Image& image, uint32_t first_row, uint32_t rows,
                          std::span<std::byte> blocks) const {
    if (format_ == BlockFormat::bc6h || options_ == nullptr ||
        !fits(format_, image.width, image.height, image.texels.size(), first_row, rows, blocks.size())) {
        return false;
    }
    const uint8_t* texels = image.texels.data();
    std::array<unsigned char, 64> rgba{};
    std::array<unsigned char, 16> red{};
    std::array<unsigned char, 16> green{};
    for (uint32_t by = first_row; by < first_row + rows; ++by) {
        for (uint32_t bx = 0; bx < blocks_across(image.width); ++bx) {
            for (uint32_t j = 0; j < BLOCK; ++j) {
                for (uint32_t i = 0; i < BLOCK; ++i) {
                    const uint8_t* texel = texels + source_of(image.width, image.height, bx, by, i, j);
                    const uint32_t at = j * BLOCK + i;
                    std::copy_n(texel, 4, rgba.data() + size_t{at} * 4);
                    red[at] = texel[0];
                    green[at] = texel[1];
                }
            }
            // CMP_Core refuses a block only when given no options, and an
            // encoder always has them.
            unsigned char* out = block_at(blocks, format_, image.width, bx, by);
            switch (format_) {
                case BlockFormat::bc4:
                    CompressBlockBC4(red.data(), BLOCK, out, options_);
                    break;
                case BlockFormat::bc5:
                    CompressBlockBC5(red.data(), BLOCK, green.data(), BLOCK, out, options_);
                    break;
                case BlockFormat::bc7:
                    CompressBlockBC7(rgba.data(), BLOCK * 4, out, options_);
                    break;
                case BlockFormat::bc6h:
                    break;
            }
        }
    }
    return true;
}

bool BlockEncoder::encode(const RgbaFloatImage& image, uint32_t first_row, uint32_t rows,
                          std::span<std::byte> blocks) const {
    if (format_ != BlockFormat::bc6h || options_ == nullptr ||
        !fits(format_, image.width, image.height, image.texels.size(), first_row, rows, blocks.size())) {
        return false;
    }
    const float* texels = image.texels.data();
    std::array<unsigned short, 48> rgb{};
    for (uint32_t by = first_row; by < first_row + rows; ++by) {
        for (uint32_t bx = 0; bx < blocks_across(image.width); ++bx) {
            for (uint32_t j = 0; j < BLOCK; ++j) {
                for (uint32_t i = 0; i < BLOCK; ++i) {
                    const float* texel = texels + source_of(image.width, image.height, bx, by, i, j);
                    const size_t at = (size_t{j} * BLOCK + i) * 3;
                    rgb[at] = to_half(texel[0]);
                    rgb[at + 1] = to_half(texel[1]);
                    rgb[at + 2] = to_half(texel[2]);
                }
            }
            CompressBlockBC6(rgb.data(), BLOCK * 3, block_at(blocks, format_, image.width, bx, by), options_);
        }
    }
    return true;
}

std::vector<uint8_t> decode(BlockFormat format, std::span<const std::byte> blocks, uint32_t width,
                            uint32_t height) {
    if (format == BlockFormat::bc6h || width == 0 || height == 0 ||
        blocks.size() != encoded_size(format, width, height)) {
        return {};
    }
    if (format == BlockFormat::bc7) fill_bc7_tables();
    std::vector<uint8_t> texels(size_t{width} * height * 4);
    std::array<unsigned char, 64> rgba{};
    std::array<unsigned char, 16> red{};
    std::array<unsigned char, 16> green{};
    for (uint32_t by = 0; by < block_rows(height); ++by) {
        for (uint32_t bx = 0; bx < blocks_across(width); ++bx) {
            const unsigned char* in = block_at(blocks, format, width, bx, by);
            switch (format) {
                case BlockFormat::bc4:
                    DecompressBlockBC4(in, red.data(), nullptr);
                    break;
                case BlockFormat::bc5:
                    DecompressBlockBC5(in, red.data(), green.data(), nullptr);
                    break;
                case BlockFormat::bc7:
                    DecompressBlockBC7(in, rgba.data(), nullptr);
                    break;
                case BlockFormat::bc6h:
                    break;
            }
            for (uint32_t j = 0; j < BLOCK && by * BLOCK + j < height; ++j) {
                for (uint32_t i = 0; i < BLOCK && bx * BLOCK + i < width; ++i) {
                    const uint32_t at = j * BLOCK + i;
                    uint8_t* texel = texels.data() + ((size_t{by} * BLOCK + j) * width + bx * BLOCK + i) * 4;
                    if (format == BlockFormat::bc7) {
                        std::copy_n(rgba.data() + size_t{at} * 4, 4, texel);
                    } else {
                        texel[0] = red[at];
                        texel[1] = format == BlockFormat::bc5 ? green[at] : uint8_t{0};
                        texel[2] = 0;
                        texel[3] = 255;
                    }
                }
            }
        }
    }
    return texels;
}

std::vector<float> decode_hdr(std::span<const std::byte> blocks, uint32_t width, uint32_t height) {
    if (width == 0 || height == 0 || blocks.size() != encoded_size(BlockFormat::bc6h, width, height)) {
        return {};
    }
    std::vector<float> texels(size_t{width} * height * 4);
    std::array<unsigned short, 48> rgb{};
    for (uint32_t by = 0; by < block_rows(height); ++by) {
        for (uint32_t bx = 0; bx < blocks_across(width); ++bx) {
            DecompressBlockBC6(block_at(blocks, BlockFormat::bc6h, width, bx, by), rgb.data(), nullptr);
            for (uint32_t j = 0; j < BLOCK && by * BLOCK + j < height; ++j) {
                for (uint32_t i = 0; i < BLOCK && bx * BLOCK + i < width; ++i) {
                    const size_t at = (size_t{j} * BLOCK + i) * 3;
                    float* texel = texels.data() + ((size_t{by} * BLOCK + j) * width + bx * BLOCK + i) * 4;
                    texel[0] = from_half(rgb[at]);
                    texel[1] = from_half(rgb[at + 1]);
                    texel[2] = from_half(rgb[at + 2]);
                    texel[3] = 1.0f;
                }
            }
        }
    }
    return texels;
}

} // namespace ftk::image
