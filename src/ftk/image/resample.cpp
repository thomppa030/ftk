#include "ftk/image/resample.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ftk::image {

namespace {

// The filter's half-width in texels of the image it makes, and the Kaiser
// window's alpha: NVIDIA's texture tools' settings, sharp without much
// ringing.
constexpr double HALF_WIDTH = 3.0;
constexpr double ALPHA = 4.0;

// The zeroth modified Bessel function of the first kind, by its series.
double bessel_i0(double x) {
    double sum = 1.0;
    double term = 1.0;
    const double quarter_square = x * x / 4.0;
    for (int k = 1; k < 64; ++k) {
        term *= quarter_square / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < sum * 1e-12) break;
    }
    return sum;
}

double sinc(double x) {
    if (std::abs(x) < 1e-9) return 1.0;
    const double angle = std::numbers::pi * x;
    return std::sin(angle) / angle;
}

// The filter at `x` texels of the image it makes from the centre.
double kaiser(double x) {
    const double t = x / HALF_WIDTH;
    if (std::abs(t) >= 1.0) return 0.0;
    return sinc(x) * bessel_i0(ALPHA * std::sqrt(1.0 - t * t)) / bessel_i0(ALPHA);
}

struct Tap {
    uint32_t source{0};
    float weight{0.0f};
};

// Each output texel's source texels and their weights, along one axis of
// `from` texels resized to `to`.
struct Taps {
    std::vector<Tap> taps{};
    std::vector<uint32_t> first{}; // where each output texel's taps start; one more at the end
};

Taps taps_for(uint32_t from, uint32_t to, Edge edge) {
    // Shrinking widens the filter by the ratio, so it averages over what one
    // output texel covers; growing keeps it at its own width.
    const double ratio = static_cast<double>(from) / static_cast<double>(to);
    const double scale = std::max(ratio, 1.0);
    const double reach = HALF_WIDTH * scale;

    Taps out;
    out.first.reserve(to + 1);
    for (uint32_t i = 0; i < to; ++i) {
        out.first.push_back(static_cast<uint32_t>(out.taps.size()));
        const double centre = (static_cast<double>(i) + 0.5) * ratio - 0.5;
        const auto lowest = static_cast<int64_t>(std::ceil(centre - reach));
        const auto highest = static_cast<int64_t>(std::floor(centre + reach));
        const size_t start = out.taps.size();
        double total = 0.0;
        for (int64_t s = lowest; s <= highest; ++s) {
            const double weight = kaiser((static_cast<double>(s) - centre) / scale);
            if (weight == 0.0) continue;
            const auto n = static_cast<int64_t>(from);
            const int64_t source = edge == Edge::wrap ? ((s % n) + n) % n : std::clamp<int64_t>(s, 0, n - 1);
            out.taps.push_back({static_cast<uint32_t>(source), static_cast<float>(weight)});
            total += weight;
        }
        for (size_t t = start; t < out.taps.size(); ++t) {
            out.taps[t].weight = static_cast<float>(out.taps[t].weight / total);
        }
    }
    out.first.push_back(static_cast<uint32_t>(out.taps.size()));
    return out;
}

} // namespace

FloatImage resize(const FloatImage& image, uint32_t width, uint32_t height, Edge edge) {
    width = std::max(width, 1U);
    height = std::max(height, 1U);
    const uint32_t channels = image.channels;
    if (image.width == 0 || image.height == 0 || channels == 0) {
        return {.texels = std::vector<float>(size_t{width} * height * channels), .width = width, .height = height,
                .channels = channels};
    }

    // Across first, into an image as wide as the result and as tall as the
    // source; then down.
    const Taps across = taps_for(image.width, width, edge);
    std::vector<float> wide(size_t{width} * image.height * channels, 0.0f);
    for (uint32_t y = 0; y < image.height; ++y) {
        const float* row = image.texels.data() + size_t{y} * image.width * channels;
        float* out_row = wide.data() + size_t{y} * width * channels;
        for (uint32_t x = 0; x < width; ++x) {
            float* out = out_row + size_t{x} * channels;
            for (uint32_t t = across.first[x]; t < across.first[x + 1]; ++t) {
                const Tap& tap = across.taps[t];
                const float* in = row + size_t{tap.source} * channels;
                for (uint32_t c = 0; c < channels; ++c) out[c] += in[c] * tap.weight;
            }
        }
    }

    const Taps down = taps_for(image.height, height, edge);
    FloatImage result{.texels = std::vector<float>(size_t{width} * height * channels, 0.0f),
                      .width = width,
                      .height = height,
                      .channels = channels};
    const size_t row_floats = size_t{width} * channels;
    for (uint32_t y = 0; y < height; ++y) {
        float* out_row = result.texels.data() + size_t{y} * row_floats;
        for (uint32_t t = down.first[y]; t < down.first[y + 1]; ++t) {
            const Tap& tap = down.taps[t];
            const float* in_row = wide.data() + size_t{tap.source} * row_floats;
            for (size_t i = 0; i < row_floats; ++i) out_row[i] += in_row[i] * tap.weight;
        }
    }
    return result;
}

} // namespace ftk::image
