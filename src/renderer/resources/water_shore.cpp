#include "renderer/resources/water_shore.hpp"

#include "renderer/resources/fjterrain.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

namespace fjell::water {

namespace {

// Ks(μ) · (4μ)^¼ over μ in [0, SHOALING_DEEP_MU], as a Chebyshev series. The
// coefficient itself runs off to infinity as the water shallows, as
// (4μ)^-¼, so what is fitted is what is left once that is taken out: smooth,
// 1 at μ = 0, and pinned at the deep end to exactly the value and slope that
// make Ks 1 and level there, so the shore meets the open sea without a step.
// Fitted by least squares to Ks solved from the dispersion relation.
constexpr std::array<float, 12> SHOALING_SERIES = {
    1.77232337f,     0.64546591f,     -0.138891131f,  0.0138438977f,
    0.0159867331f,   -0.0109566152f,  0.00176291831f, 0.00176656106f,
    -0.00124069152f, 0.00015039643f,  0.00026573817f, -0.000150464417f,
};

// The same series differentiated in μ.
constexpr std::array<float, 11> SHOALING_SERIES_SLOPE = {
    0.184079677f,     -0.120300554f,  -0.000678283337f, 0.0384321697f,
    -0.0244106799f,   0.00189106248f, 0.00689393515f,   -0.00415322883f,
    -0.000172308952f, 0.00151850376f, -0.000945776294f,
};

// A Chebyshev series at t in [-1, 1], by Clenshaw's recurrence: the stable
// way to sum one in floats.
[[nodiscard]] float chebyshev(std::span<const float> series, float t) {
    const float two_t = 2.0f * t;
    float b1 = 0.0f;
    float b2 = 0.0f;
    for (std::size_t i = series.size() - 1; i > 0; --i) {
        const float b0 = (two_t * b1) - b2 + series[i];
        b2 = b1;
        b1 = b0;
    }
    return (t * b1) - b2 + series[0];
}

} // namespace

Shoaling shoaling_at(float mu) {
    if (mu >= SHOALING_DEEP_MU) return {};

    const bool held = mu < SHOALING_SHALLOWEST_MU;
    const float m = held ? SHOALING_SHALLOWEST_MU : mu;
    const float t = (m * (2.0f / SHOALING_DEEP_MU)) - 1.0f;
    const float stretch = std::sqrt(std::sqrt(4.0f * m));   // (4μ)^¼

    Shoaling s;
    s.coefficient = chebyshev(SHOALING_SERIES, t) / stretch;
    s.slope = held ? 0.0f
                   : (chebyshev(SHOALING_SERIES_SLOPE, t) / stretch) - (s.coefficient / (4.0f * m));
    return s;
}

std::optional<ShoreEffect> shore_effect(const StillDepth& still, ShoalingSum gerstner,
                                        ShoalingSum simulated) {
    // A sum with no height has nothing to shoal, and no say in where the
    // shallows begin.
    const bool has_gerstner = gerstner.amplitude > 0.0f && gerstner.k > 0.0f;
    const bool has_simulated = simulated.amplitude > 0.0f && simulated.k > 0.0f;
    const float d = still.depth;
    const bool gerstner_deep = !has_gerstner || gerstner.k * d >= SHOALING_DEEP_MU;
    const bool simulated_deep = !has_simulated || simulated.k * d >= SHOALING_DEEP_MU;
    if (gerstner_deep && simulated_deep) return std::nullopt;

    ShoreEffect e;
    if (d <= 0.0f) {
        // The bed stands above the water: no sea here to shape.
        e.gerstner = 0.0f;
        e.simulated = 0.0f;
        e.breaking = 1.0f;
        return e;
    }

    const Shoaling g = has_gerstner ? shoaling_at(gerstner.k * d) : Shoaling{};
    const Shoaling s = has_simulated ? shoaling_at(simulated.k * d) : Shoaling{};

    // Each sum's height as it has shoaled, and how that changes with depth.
    const float g_height = has_gerstner ? g.coefficient * gerstner.amplitude : 0.0f;
    const float s_height = has_simulated ? s.coefficient * simulated.amplitude : 0.0f;
    const float g_rate = has_gerstner ? g.slope * gerstner.k * gerstner.amplitude : 0.0f;
    const float s_rate = has_simulated ? s.slope * simulated.k * simulated.amplitude : 0.0f;

    // The water's height is the two together, as variances add; breaking
    // holds that to what the depth can carry, and whatever share it keeps,
    // both sums keep.
    const float height = std::sqrt((g_height * g_height) + (s_height * s_height));
    const float height_rate = ((g_height * g_rate) + (s_height * s_rate)) / height;
    const float carried = 0.5f * BREAKER_INDEX * d;
    float kept = 1.0f;
    float kept_rate = 0.0f;
    if (height > carried) {
        kept = carried / height;
        kept_rate = ((0.5f * BREAKER_INDEX) - (kept * height_rate)) / height;
    }

    // factor = Ks · kept, and its change with depth is carried across the
    // point by the depth's own slope.
    e.gerstner = g.coefficient * kept;
    e.gerstner_gradient =
        ((g.slope * gerstner.k * kept) + (g.coefficient * kept_rate)) * still.gradient;
    e.simulated = s.coefficient * kept;
    e.simulated_gradient =
        ((s.slope * simulated.k * kept) + (s.coefficient * kept_rate)) * still.gradient;
    e.breaking = 1.0f - kept;
    return e;
}

std::optional<StillDepth> still_depth(const ShoreBed& bed, glm::vec2 world_xz) {
    if (bed.terrain == nullptr) return std::nullopt;
    const FjTerrainData& terrain = *bed.terrain;
    const std::uint32_t samples = terrain.sample_count();
    const float size = terrain.header.world_size;
    if (samples < 2 || size <= 0.0f
        || terrain.heights.size() < static_cast<std::size_t>(samples) * samples) {
        return std::nullopt;
    }

    // Field coordinates, metres from the min corner. Off the field there is
    // no bed to speak of, rather than the edge's height carried on outward.
    const glm::vec2 field = world_xz - bed.min_corner;
    if (field.x < 0.0f || field.y < 0.0f || field.x > size || field.y > size) {
        return std::nullopt;
    }

    const auto quads = static_cast<float>(samples - 1);
    const float per_metre = quads / size;
    const float fx = std::min(field.x * per_metre, quads);
    const float fz = std::min(field.y * per_metre, quads);
    // The last row and column start no cell of their own, so a point on the
    // far edge reads the cell before it, at its far side.
    const auto x0 = std::min(static_cast<std::uint32_t>(fx), samples - 2);
    const auto z0 = std::min(static_cast<std::uint32_t>(fz), samples - 2);
    const float tx = fx - static_cast<float>(x0);
    const float tz = fz - static_cast<float>(z0);

    const auto at = [&](std::uint32_t x, std::uint32_t z) {
        return terrain.heights[(static_cast<std::size_t>(z) * samples) + x];
    };
    const float h00 = at(x0, z0);
    const float h10 = at(x0 + 1, z0);
    const float h01 = at(x0, z0 + 1);
    const float h11 = at(x0 + 1, z0 + 1);

    const float top = h00 + ((h10 - h00) * tx);
    const float bottom = h01 + ((h11 - h01) * tx);
    const float normalized = top + ((bottom - top) * tz);

    // The bed rises as the normalized height does; the depth falls with it.
    const float rise = terrain.header.height_scale * per_metre;
    const float slope_x = ((h10 - h00) * (1.0f - tz)) + ((h11 - h01) * tz);
    const float slope_z = bottom - top;

    StillDepth out;
    out.depth = bed.surface_y - (bed.ground_y + terrain.world_height(normalized));
    out.gradient = glm::vec2(-slope_x, -slope_z) * rise;
    return out;
}

} // namespace fjell::water
