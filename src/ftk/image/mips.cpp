#include "ftk/image/mips.hpp"

#include "ftk/math/color_space.hpp"

#include <algorithm>
#include <cmath>

namespace ftk::image {

namespace {

// Which channel is alpha: the fourth of four, the second of two.
std::optional<uint32_t> alpha_channel(uint32_t channels) {
    if (channels == 4) return 3U;
    if (channels == 2) return 1U;
    return std::nullopt;
}

// The image as its mips are filtered: sRGB colour made linear, a normal map's
// channels made -1 to 1.
FloatImage to_filtered(const FloatImage& image, MipContent content) {
    FloatImage out = image;
    const std::optional<uint32_t> alpha = alpha_channel(image.channels);
    for (size_t i = 0; i < out.texels.size(); ++i) {
        const auto channel = static_cast<uint32_t>(i % image.channels);
        if (channel == alpha) continue;
        float& value = out.texels[i];
        if (content == MipContent::srgb_color) value = color_space::srgb_to_linear(value);
        if (content == MipContent::normal_map && channel < 3) value = value * 2.0f - 1.0f;
    }
    return out;
}

// Makes the first three channels of every texel unit length; a texel that
// filtered to nothing faces straight out.
void normalize_normals(FloatImage& image) {
    for (size_t i = 0; i + 2 < image.texels.size(); i += image.channels) {
        float* n = image.texels.data() + i;
        const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (length < 1e-6f) {
            n[0] = 0.0f;
            n[1] = 0.0f;
            n[2] = 1.0f;
        } else {
            n[0] /= length;
            n[1] /= length;
            n[2] /= length;
        }
    }
}

// A filtered mip back as the image holds its texels.
FloatImage to_stored(FloatImage image, MipContent content) {
    if (content == MipContent::linear) return image;
    const std::optional<uint32_t> alpha = alpha_channel(image.channels);
    for (size_t i = 0; i < image.texels.size(); ++i) {
        const auto channel = static_cast<uint32_t>(i % image.channels);
        float& value = image.texels[i];
        if (channel == alpha) {
            value = std::clamp(value, 0.0f, 1.0f);
        } else if (content == MipContent::srgb_color) {
            value = color_space::linear_to_srgb(std::clamp(value, 0.0f, 1.0f));
        } else if (channel < 3) {
            value = std::clamp(value * 0.5f + 0.5f, 0.0f, 1.0f);
        }
    }
    return image;
}

float coverage_at(const FloatImage& image, uint32_t alpha, float cutoff, float scale) {
    size_t covered = 0;
    for (size_t i = alpha; i < image.texels.size(); i += image.channels) {
        if (std::min(image.texels[i] * scale, 1.0f) >= cutoff) ++covered;
    }
    return static_cast<float>(covered) / static_cast<float>(size_t{image.width} * image.height);
}

// Scales `image`'s alpha by the least factor that brings the share of texels
// at or above `cutoff` up to `target`. More coverage only comes with a larger
// factor, so the factor is found by halving the range it lies in.
void keep_coverage(FloatImage& image, uint32_t alpha, float cutoff, float target) {
    float low = 0.0f;
    float high = 1.0f;
    constexpr float LARGEST_SCALE = 1024.0f;
    while (coverage_at(image, alpha, cutoff, high) < target && high < LARGEST_SCALE) high *= 2.0f;
    for (int step = 0; step < 24; ++step) {
        const float middle = (low + high) / 2.0f;
        if (coverage_at(image, alpha, cutoff, middle) >= target) {
            high = middle;
        } else {
            low = middle;
        }
    }
    for (size_t i = alpha; i < image.texels.size(); i += image.channels) {
        image.texels[i] = std::min(image.texels[i] * high, 1.0f);
    }
}

} // namespace

float alpha_coverage(const FloatImage& image, float cutoff) {
    const std::optional<uint32_t> alpha = alpha_channel(image.channels);
    if (!alpha || image.width == 0 || image.height == 0) return 1.0f;
    return coverage_at(image, *alpha, cutoff, 1.0f);
}

std::vector<FloatImage> make_mips(const FloatImage& image, const MipOptions& options) {
    std::vector<FloatImage> mips;
    if (image.width == 0 || image.height == 0) return mips;
    const std::optional<uint32_t> alpha = alpha_channel(image.channels);
    const std::optional<float> cutout = alpha ? options.cutout : std::nullopt;
    const float target = cutout ? alpha_coverage(image, *cutout) : 0.0f;

    // Each mip is filtered from the one before as it was filtered, never from
    // its stored form, so the colour space and the coverage scaling are each
    // applied once.
    FloatImage filtered = to_filtered(image, options.content);
    while (filtered.width > 1 || filtered.height > 1) {
        filtered = resize(filtered, std::max(filtered.width / 2, 1U), std::max(filtered.height / 2, 1U), options.edge);
        if (options.content == MipContent::normal_map) normalize_normals(filtered);
        FloatImage mip = to_stored(filtered, options.content);
        if (cutout) keep_coverage(mip, *alpha, *cutout, target);
        mips.push_back(std::move(mip));
    }
    return mips;
}

} // namespace ftk::image
