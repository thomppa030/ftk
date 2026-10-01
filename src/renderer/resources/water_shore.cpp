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

std::optional<ShoreEffect> shore_effect(const std::optional<StillDepth>& still,
                                        const ExposureAt& exposure, ShoalingSum gerstner,
                                        ShoalingSum simulated) {
    // A sum with no height has nothing to shoal, and no say in where the
    // shallows begin. One present is left alone where it arrives whole and
    // the water is too deep for it to feel the bed.
    const bool has_gerstner = gerstner.amplitude > 0.0f && gerstner.k > 0.0f;
    const bool has_simulated = simulated.amplitude > 0.0f && simulated.k > 0.0f;
    const auto untouched = [&](bool has, ShoalingSum sum, float share) {
        if (!has) return true;
        if (share < 1.0f) return false;
        return !still.has_value() || sum.k * still->depth >= SHOALING_DEEP_MU;
    };
    if (untouched(has_gerstner, gerstner, exposure.trains)
        && untouched(has_simulated, simulated, exposure.simulated)) {
        return std::nullopt;
    }

    ShoreEffect e;
    if (still.has_value() && still->depth <= 0.0f) {
        // The bed stands above the water: no sea here to shape.
        e.gerstner = 0.0f;
        e.simulated = 0.0f;
        e.breaking = 1.0f;
        return e;
    }

    // Unknown depth is deep water: it neither shoals nor breaks.
    const float d = still.has_value() ? still->depth : 0.0f;
    const glm::vec2 depth_slope = still.has_value() ? still->gradient : glm::vec2(0.0f);
    const Shoaling g = has_gerstner && still.has_value() ? shoaling_at(gerstner.k * d) : Shoaling{};
    const Shoaling s =
        has_simulated && still.has_value() ? shoaling_at(simulated.k * d) : Shoaling{};
    const float g_share = has_gerstner ? exposure.trains : 1.0f;
    const float s_share = has_simulated ? exposure.simulated : 1.0f;
    const glm::vec2 g_share_slope = has_gerstner ? exposure.trains_gradient : glm::vec2(0.0f);
    const glm::vec2 s_share_slope = has_simulated ? exposure.simulated_gradient : glm::vec2(0.0f);

    // Each sum before breaking, as a factor on its amplitude and how that
    // changes across the point: what gets through, grown by shoaling.
    const float g_grown = g.coefficient * g_share;
    const float s_grown = s.coefficient * s_share;
    const glm::vec2 g_grown_slope =
        (g.slope * gerstner.k * g_share * depth_slope) + (g.coefficient * g_share_slope);
    const glm::vec2 s_grown_slope =
        (s.slope * simulated.k * s_share * depth_slope) + (s.coefficient * s_share_slope);

    // The water's height is the two together, as variances add; breaking
    // holds that to what the depth can carry, and whatever share it keeps,
    // both sums keep.
    float kept = 1.0f;
    glm::vec2 kept_slope{0.0f};
    if (still.has_value()) {
        const float g_height = has_gerstner ? g_grown * gerstner.amplitude : 0.0f;
        const float s_height = has_simulated ? s_grown * simulated.amplitude : 0.0f;
        const float height = std::sqrt((g_height * g_height) + (s_height * s_height));
        const float carried = 0.5f * BREAKER_INDEX * d;
        if (height > carried) {
            const glm::vec2 g_height_slope =
                has_gerstner ? g_grown_slope * gerstner.amplitude : glm::vec2(0.0f);
            const glm::vec2 s_height_slope =
                has_simulated ? s_grown_slope * simulated.amplitude : glm::vec2(0.0f);
            const glm::vec2 height_slope =
                ((g_height * g_height_slope) + (s_height * s_height_slope)) / height;
            kept = carried / height;
            kept_slope = ((0.5f * BREAKER_INDEX * depth_slope) - (kept * height_slope)) / height;
        }
    }

    e.gerstner = g_grown * kept;
    e.gerstner_gradient = (g_grown_slope * kept) + (g_grown * kept_slope);
    e.simulated = s_grown * kept;
    e.simulated_gradient = (s_grown_slope * kept) + (s_grown * kept_slope);
    e.breaking = 1.0f - kept;
    return e;
}

std::optional<SwashPlan> swash_plan(const ShoreBed& bed, ShoalingSum gerstner,
                                    ShoalingSum simulated, glm::vec2 world_xz) {
    const std::optional<StillDepth> still = still_depth(bed, world_xz);
    if (!still.has_value()) return std::nullopt;
    const bool has_gerstner = gerstner.amplitude > 0.0f && gerstner.k > 0.0f;
    const bool has_simulated = simulated.amplitude > 0.0f && simulated.k > 0.0f;
    if (!has_gerstner && !has_simulated) return std::nullopt;

    // The sea as it reaches this beach: through whatever the land lets past,
    // its significant height and the wavelength its energy is carried at.
    const ExposureAt open =
        bed.exposure != nullptr ? exposure_at(*bed.exposure, world_xz) : ExposureAt{};
    const float g_amplitude = has_gerstner ? gerstner.amplitude * open.trains : 0.0f;
    const float s_amplitude = has_simulated ? simulated.amplitude * open.simulated : 0.0f;
    const float g_energy = g_amplitude * g_amplitude;
    const float s_energy = s_amplitude * s_amplitude;
    if (g_energy + s_energy <= 0.0f) return std::nullopt;
    constexpr float TAU = 6.28318530717958647692f;
    const float height = 2.0f * std::sqrt(g_energy + s_energy);
    const float wavelength = ((has_gerstner ? g_energy * TAU / gerstner.k : 0.0f)
                              + (has_simulated ? s_energy * TAU / simulated.k : 0.0f))
                           / (g_energy + s_energy);

    // Out past where it breaks the sea is the sea's; level ground has no way
    // up the beach to run.
    const float d = still->depth;
    const float breaks_at = height / BREAKER_INDEX;
    const float slope = glm::length(still->gradient);
    if (d >= breaks_at || slope < 1e-4f) return std::nullopt;

    const float beach = std::min(slope, SWASH_STEEPEST_BEACH);
    const float root = std::sqrt(height * wavelength);
    const float full_setup = 0.35f * beach * root;
    const float full_swing = 0.5f * root * std::sqrt((0.563f * beach * beach) + 0.004f);
    // Sand higher than the swash can ever reach keeps the water under it.
    if (-d > full_setup + full_swing) return std::nullopt;

    SwashPlan plan;
    plan.seaward = still->gradient / slope;
    plan.waterline = world_xz - (plan.seaward * (d / slope));

    // Setup across the surf, all of it at the waterline and above.
    plan.setup = full_setup * (d <= 0.0f ? 1.0f : 1.0f - (d / breaks_at));
    plan.setup_gradient = d <= 0.0f ? glm::vec2(0.0f) : -full_setup * still->gradient / breaks_at;

    // The swing at the waterline, gone in water twice as deep as it.
    const float swing_depth = std::min(std::max(2.0f * full_swing, 0.25f), breaks_at);
    plan.share = d <= 0.0f ? 1.0f : std::max(1.0f - (d / swing_depth), 0.0f);
    plan.swing = full_swing * plan.share;
    plan.swing_gradient = d <= 0.0f || d >= swing_depth
        ? glm::vec2(0.0f)
        : -full_swing * still->gradient / swing_depth;

    const float raw = std::sqrt((has_gerstner ? gerstner.amplitude * gerstner.amplitude : 0.0f)
                                + (has_simulated ? simulated.amplitude * simulated.amplitude
                                                 : 0.0f));
    plan.scale = 1.0f / raw;
    return plan;
}

Swash swash_from(const SwashPlan& plan, float sea_height, glm::vec2 sea_slope, float sea_rate) {
    const float arriving = sea_height * plan.scale;
    const float share = std::clamp(arriving, -1.0f, 1.0f);
    const bool held = arriving != share;

    Swash out;
    out.lift = plan.setup + (plan.swing * share);
    out.gradient = plan.setup_gradient + (plan.swing_gradient * share);
    if (!held) {
        const glm::vec2 along = sea_slope - (plan.seaward * glm::dot(plan.seaward, sea_slope));
        out.gradient += plan.swing * plan.scale * along;
        out.rate = plan.swing * plan.scale * sea_rate;
    }
    return out;
}

ExposureAt exposure_at(const ShoreExposure& exposure, glm::vec2 world_xz) {
    ExposureAt out;
    const std::uint32_t n = exposure.samples;
    if (n < 2 || exposure.size <= 0.0f
        || exposure.shares.size() < 2 * static_cast<std::size_t>(n) * n) {
        return out;
    }
    const glm::vec2 field = world_xz - exposure.min_corner;
    if (field.x < 0.0f || field.y < 0.0f || field.x > exposure.size || field.y > exposure.size) {
        return out;
    }

    // As still_depth() reads the bed: the cell's four samples, blended.
    const auto quads = static_cast<float>(n - 1);
    const float per_metre = quads / exposure.size;
    const float fx = std::min(field.x * per_metre, quads);
    const float fz = std::min(field.y * per_metre, quads);
    const auto x0 = std::min(static_cast<std::uint32_t>(fx), n - 2);
    const auto z0 = std::min(static_cast<std::uint32_t>(fz), n - 2);
    const float tx = fx - static_cast<float>(x0);
    const float tz = fz - static_cast<float>(z0);

    const auto at = [&](std::uint32_t x, std::uint32_t z, std::uint32_t channel) {
        return static_cast<float>(
                   exposure.shares[(2 * ((static_cast<std::size_t>(z) * n) + x)) + channel])
             / 65535.0f;
    };
    const auto blend = [&](std::uint32_t channel, float& value, glm::vec2& slope) {
        const float v00 = at(x0, z0, channel);
        const float v10 = at(x0 + 1, z0, channel);
        const float v01 = at(x0, z0 + 1, channel);
        const float v11 = at(x0 + 1, z0 + 1, channel);
        const float top = v00 + ((v10 - v00) * tx);
        const float bottom = v01 + ((v11 - v01) * tx);
        value = top + ((bottom - top) * tz);
        slope = glm::vec2(((v10 - v00) * (1.0f - tz)) + ((v11 - v01) * tz), bottom - top)
              * per_metre;
    };
    blend(0, out.trains, out.trains_gradient);
    blend(1, out.simulated, out.simulated_gradient);
    return out;
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
