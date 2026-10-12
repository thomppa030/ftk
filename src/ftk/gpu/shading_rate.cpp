#include "ftk/gpu/shading_rate.hpp"

#include <cstddef>

namespace ftk::gpu {

namespace {

constexpr std::array<uint8_t, 3> SIDES{1, 2, 4};

// A side's place in the table, its log2; -1 for a side no GPU offers.
int side_index(uint8_t side) {
    for (int i = 0; i < static_cast<int>(SIDES.size()); ++i) {
        if (SIDES[static_cast<size_t>(i)] == side) return i;
    }
    return -1;
}

} // namespace

bool ShadingRates::offers(ShadingRate rate, Samples samples) const {
    if (rate == ShadingRate{}) return true;
    const int w = side_index(rate.width);
    const int h = side_index(rate.height);
    if (w < 0 || h < 0) return false;
    return (offered_at[static_cast<size_t>(w)][static_cast<size_t>(h)] & static_cast<uint8_t>(samples)) != 0;
}

void ShadingRates::offer(ShadingRate rate, Samples samples) {
    const int w = side_index(rate.width);
    const int h = side_index(rate.height);
    if (w < 0 || h < 0) return;
    offered_at[static_cast<size_t>(w)][static_cast<size_t>(h)] |= static_cast<uint8_t>(samples);
}

ShadingRate rate_for(const ShadingRates& rates, Samples samples, ShadingRate asked) {
    ShadingRate best{};
    for (const uint8_t w : SIDES) {
        for (const uint8_t h : SIDES) {
            const ShadingRate rate{w, h};
            if (w > asked.width || h > asked.height || !rates.offers(rate, samples)) continue;
            const int area = w * h;
            const int best_area = best.width * best.height;
            if (area < best_area) continue;
            if (area == best_area) {
                const int sum = w + h;
                const int best_sum = best.width + best.height;
                if (sum > best_sum) continue;
                if (sum == best_sum && w <= best.width) continue;
            }
            best = rate;
        }
    }
    return best;
}

} // namespace ftk::gpu
