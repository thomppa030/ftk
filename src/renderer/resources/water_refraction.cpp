#include "renderer/resources/water_refraction.hpp"

#include "core/portable_math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace fjell::water {

namespace {

constexpr double UNREACHED = std::numeric_limits<double>::infinity();

// The most passes of four sweeps the arrival is given to settle. Each pass
// carries a front through every turn it takes from one quarter of the compass
// into the next, so a fjord would need one for each of its bends.
constexpr int MAX_SWEEPS = 64;

// (k / k_deep) * sqrt(mu) over mu in [0, SHOALING_DEEP_MU], as a Chebyshev
// series, where mu = k_deep * d and k comes from the dispersion relation
// x tanh(x) = mu, x = k d. The ratio itself runs off to infinity as the water
// shallows, as 1 / sqrt(mu), so what is fitted is what is left once that is
// taken out: smooth, 1 at mu = 0, and pinned at the deep end to sqrt of it, so
// the ratio is exactly 1 where the sea counts as deep. Fitted by least
// squares to the relation solved directly; within 7e-6 of it wherever the
// ratio is under MAX_SHORTENING.
constexpr std::array<float, 13> SHORTENING_SERIES = {
    1.83677852f,      0.848780692f,     -0.0241584145f,  -0.0255942829f,
    0.0117992051f,    -0.000964379753f, -0.00157152792f, 0.000806956552f,
    -1.85162335e-05f, -0.000158622643f, 6.91958849e-05f, 3.67324469e-06f,
    -2.12423183e-05f,
};

// Where a front can go on the warp's grid: the samples in water, and between
// each and the next along x and along z, whether the ground between them is
// water too. The warp's samples lie twice as far apart as the land's marks,
// so a spit narrower than a warp cell is still seen at the mark between them.
struct Passage {
    std::uint32_t samples{0};
    std::vector<std::uint8_t> wet;
    std::vector<std::uint8_t> next_x;
    std::vector<std::uint8_t> next_z;
};

[[nodiscard]] Passage passage_over(const ShelterLand& land) {
    Passage out;
    const std::uint32_t m = land.warp_samples;
    out.samples = m;
    const std::size_t count = static_cast<std::size_t>(m) * m;
    out.wet.assign(count, 0);
    out.next_x.assign(count, 0);
    out.next_z.assign(count, 0);

    const std::uint32_t n = land.samples;
    const bool marked = n >= 2 && land.land.size() == static_cast<std::size_t>(n) * n;
    const double per_mark = marked ? static_cast<double>(n - 1) / static_cast<double>(m - 1) : 0.0;
    // Land marked at a point given in warp cells.
    const auto land_at = [&](double x, double z) {
        if (!marked) return false;
        const auto mark = [&](double cells) {
            return std::min(static_cast<std::uint32_t>(std::lround(cells * per_mark)), n - 1);
        };
        return land.land[(static_cast<std::size_t>(mark(z)) * n) + mark(x)] != 0;
    };

    for (std::uint32_t z = 0; z < m; ++z) {
        for (std::uint32_t x = 0; x < m; ++x) {
            const std::size_t i = (static_cast<std::size_t>(z) * m) + x;
            out.wet[i] = land.warp_depths[i] > 0.0f && !land_at(x, z) ? 1 : 0;
        }
    }
    for (std::uint32_t z = 0; z < m; ++z) {
        for (std::uint32_t x = 0; x < m; ++x) {
            const std::size_t i = (static_cast<std::size_t>(z) * m) + x;
            if (out.wet[i] == 0) continue;
            if (x + 1 < m && out.wet[i + 1] != 0 && !land_at(x + 0.5, z)) out.next_x[i] = 1;
            if (z + 1 < m && out.wet[i + m] != 0 && !land_at(x, z + 0.5)) out.next_z[i] = 1;
        }
    }
    return out;
}

} // namespace

float shortening_at(float mu) {
    if (mu <= 0.0f || mu >= SHOALING_DEEP_MU) return 1.0f;
    const float t = (2.0f * mu / SHOALING_DEEP_MU) - 1.0f;
    return std::min(portable::chebyshev(SHORTENING_SERIES, t) / std::sqrt(mu), MAX_SHORTENING);
}

std::vector<double> arrival(const ShelterLand& land, const WarpSource& dominant) {
    const std::uint32_t m = land.warp_samples;
    const std::size_t count = static_cast<std::size_t>(m) * m;
    std::vector<double> at(count, UNREACHED);
    if (m < 2 || land.warp_depths.size() != count || dominant.k <= 0.0f || land.size <= 0.0f) {
        return at;
    }
    const Passage passage = passage_over(land);
    const double cell = static_cast<double>(land.size) / static_cast<double>(m - 1);
    const glm::dvec2 travel = glm::normalize(glm::dvec2(dominant.travel));
    const glm::dvec2 corner(land.min_corner);

    // Metres of the deep sea's travel to each metre a front makes here: how
    // many times shorter the wave is, since it gathers that many times the
    // deep sea's phase a metre.
    std::vector<double> slowness(count, 1.0);
    for (std::size_t i = 0; i < count; ++i) {
        if (passage.wet[i] != 0) slowness[i] = shortening_at(dominant.k * land.warp_depths[i]);
    }

    // The sea comes in from past the grid as the plane wave it is offshore,
    // at every edge sample facing it or running alongside it: deep water,
    // since the grid reaches past the terrain on every side.
    std::vector<std::uint8_t> entered(count, 0);
    const auto enter = [&](std::uint32_t x, std::uint32_t z, glm::dvec2 outward) {
        const std::size_t i = (static_cast<std::size_t>(z) * m) + x;
        if (passage.wet[i] == 0 || glm::dot(travel, outward) > 0.0) return;
        at[i] = glm::dot(travel, corner + (glm::dvec2(x, z) * cell));
        entered[i] = 1;
    };
    for (std::uint32_t s = 0; s < m; ++s) {
        enter(0, s, {-1.0, 0.0});
        enter(m - 1, s, {1.0, 0.0});
        enter(s, 0, {0.0, -1.0});
        enter(s, m - 1, {0.0, 1.0});
    }

    // Whether a front passes between two samples next to each other along x,
    // or along z: `i` and the one after it.
    const auto open_x = [&](std::size_t i) { return passage.next_x[i] != 0; };
    const auto open_z = [&](std::size_t i) { return passage.next_z[i] != 0; };
    // ... or across a cell's diagonal, by either of the two ways round it.
    const auto open_rising = [&](std::size_t i) {   // i to i + m + 1
        return (open_x(i) && open_z(i + 1)) || (open_z(i) && open_x(i + m));
    };
    const auto open_falling = [&](std::size_t i) {   // i + 1 to i + m
        return (open_x(i) && open_z(i)) || (open_z(i + 1) && open_x(i + m));
    };

    // Of two neighbours on opposite sides of a sample, the one a front
    // reached first, and how slow the ground is there.
    struct Earlier {
        double at{UNREACHED};
        double slowness{1.0};
    };
    const auto earlier = [&](Earlier best, bool open, std::size_t j) {
        return open && at[j] < best.at ? Earlier{at[j], slowness[j]} : best;
    };

    // By the upwind (Godunov) rule, the arrival at a sample `spacing` metres
    // from the earlier of its neighbours along each of two axes at right
    // angles: of a front through both of them, or through the earlier alone
    // when the other is too late to be on the same front. Over each step the
    // ground is as slow as it is on average between where the front leaves
    // and here, which keeps the arrival true where the depth changes within a
    // cell.
    const auto through = [&](Earlier a, Earlier b, double own, double spacing) {
        if (a.at > b.at) std::swap(a, b);
        if (a.at == UNREACHED) return UNREACHED;
        const double alone = a.at + (0.5 * (own + a.slowness) * spacing);
        const double gap = b.at - a.at;
        const double step = 0.5 * (own + (0.5 * (a.slowness + b.slowness))) * spacing;
        if (gap >= step) return alone;
        return std::min(alone, 0.5 * (a.at + b.at + std::sqrt((2.0 * step * step) - (gap * gap))));
    };

    // Samples that may arrive earlier than they did when last settled: all
    // of them at first, and after that the neighbours of any that did. Over
    // the shallows, where the rays turn all the way, the arrival takes many
    // passes to settle, but by the later ones only a few samples still move.
    std::vector<std::uint8_t> unsettled(count, 1);

    // One sample from its neighbours: along the grid's axes, and along its
    // diagonals as another pair of axes at right angles, so a front fanning
    // out round the end of land keeps its round shape rather than taking the
    // grid's square one.
    const double diagonal = cell * std::sqrt(2.0);
    const auto settle = [&](std::uint32_t x, std::uint32_t z) {
        const std::size_t i = (static_cast<std::size_t>(z) * m) + x;
        if (unsettled[i] == 0) return false;
        unsettled[i] = 0;
        if (passage.wet[i] == 0 || entered[i] != 0) return false;
        const bool left = x > 0;
        const bool right = x + 1 < m;
        const bool below = z > 0;
        const bool above = z + 1 < m;

        Earlier along_x = earlier({}, left && open_x(i - 1), i - 1);
        along_x = earlier(along_x, right && open_x(i), i + 1);
        Earlier along_z = earlier({}, below && open_z(i - m), i - m);
        along_z = earlier(along_z, above && open_z(i), i + m);

        Earlier rising = earlier({}, left && below && open_rising(i - m - 1), i - m - 1);
        rising = earlier(rising, right && above && open_rising(i), i + m + 1);
        Earlier falling = earlier({}, right && below && open_falling(i - m), i - m + 1);
        falling = earlier(falling, left && above && open_falling(i - 1), i + m - 1);

        const double reached = std::min(through(along_x, along_z, slowness[i], cell),
                                        through(rising, falling, slowness[i], diagonal));
        // A metre in a million: what is left once the sweeps have settled is
        // the last bits of a double going back and forth.
        if (reached >= at[i] - 1e-6) return false;
        at[i] = reached;
        for (std::uint32_t nz = below ? z - 1 : z; nz <= (above ? z + 1 : z); ++nz) {
            for (std::uint32_t nx = left ? x - 1 : x; nx <= (right ? x + 1 : x); ++nx) {
                unsettled[(static_cast<std::size_t>(nz) * m) + nx] = 1;
            }
        }
        return true;
    };

    // Sweeps in each of the four diagonal orders, so a front travelling any
    // way is carried across the grid in one, until a pass changes nothing.
    const std::uint32_t last = m - 1;
    for (int pass = 0; pass < MAX_SWEEPS; ++pass) {
        bool changed = false;
        for (int order = 0; order < 4; ++order) {
            const bool x_down = (order & 1) != 0;
            const bool z_down = (order & 2) != 0;
            for (std::uint32_t zs = 0; zs < m; ++zs) {
                const std::uint32_t z = z_down ? last - zs : zs;
                for (std::uint32_t xs = 0; xs < m; ++xs) {
                    changed |= settle(x_down ? last - xs : xs, z);
                }
            }
        }
        if (!changed) break;
    }
    return at;
}

} // namespace fjell::water
