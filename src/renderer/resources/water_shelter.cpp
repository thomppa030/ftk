#include "renderer/resources/water_shelter.hpp"

#include "core/portable_math.hpp"
#include "core/thread_pool.hpp"
#include "renderer/resources/fjterrain.hpp"
#include "renderer/resources/fjwater.hpp"
#include "renderer/resources/water_refraction.hpp"
#include "renderer/resources/water_spectrum.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>

namespace fjell::water {

namespace {

constexpr float TAU = 6.28318530717958647692f;

// The steps sources are taken in (see shelter_sources()).
constexpr float HEADING_STEP_DEGREES = 1.0f;
constexpr float WAVELENGTH_STEP = 1.05f;
constexpr float WEIGHT_STEP = 0.01f;

// The wavelengths a source steps to: WAVELENGTH_STEP to the powers from
// FIRST_WAVELENGTH_POWER up, 5 cm to 17 km. Built by repeated division and
// multiplication, which every machine rounds alike, rather than by exp and
// log, which they do not; and with them, the wavelengths halfway between each
// and the next by ratio, sqrt(WAVELENGTH_STEP) up from it.
constexpr int FIRST_WAVELENGTH_POWER = -60;
constexpr std::size_t WAVELENGTH_STEPS = 261;
constexpr float HALF_WAVELENGTH_STEP = 1.02469507659596f;
struct WavelengthSteps {
    std::array<float, WAVELENGTH_STEPS> at{};
    std::array<float, WAVELENGTH_STEPS - 1> between{};
};
constexpr WavelengthSteps WAVELENGTHS = [] {
    WavelengthSteps steps;
    float wavelength = 1.0f;
    for (int i = 0; i > FIRST_WAVELENGTH_POWER; --i) wavelength /= WAVELENGTH_STEP;
    for (std::size_t i = 0; i < WAVELENGTH_STEPS; ++i) {
        steps.at[i] = wavelength;
        if (i + 1 < WAVELENGTH_STEPS) steps.between[i] = wavelength * HALF_WAVELENGTH_STEP;
        wavelength *= WAVELENGTH_STEP;
    }
    return steps;
}();

// How far past the terrain the grid reaches on each side, as a share of the
// terrain's size: room for a lee cast near the edge to fade out rather than
// stop.
constexpr float MARGIN_SHARE = 0.25f;

// The depth the warp's grid records where there is no bed: deep enough for any
// wave.
constexpr float DEEP = 1e6f;

// Back to nothing across the margin: the share of a sample's value kept, by
// how far it lies inside the grid's edge.
[[nodiscard]] float kept_inside(std::uint32_t edge_samples, float cell, float margin) {
    if (margin <= 0.0f) return 1.0f;
    const float into = std::clamp(static_cast<float>(edge_samples) * cell / margin, 0.0f, 1.0f);
    return into * into * (3.0f - (2.0f * into));
}

// The way along `angle` radians from the x axis.
[[nodiscard]] glm::vec2 heading_of(float angle) {
    const portable::SinCos turn = portable::sincos(angle);
    return {turn.cos, turn.sin};
}

[[nodiscard]] glm::vec2 stepped_heading(glm::vec2 travel) {
    const float degrees = glm::degrees(portable::atan2(travel.y, travel.x));
    return heading_of(
        glm::radians(std::round(degrees / HEADING_STEP_DEGREES) * HEADING_STEP_DEGREES));
}

// The nearest of WAVELENGTHS, nearest by ratio.
[[nodiscard]] float stepped_wavelength(float wavelength) {
    const auto above = std::upper_bound(WAVELENGTHS.between.begin(), WAVELENGTHS.between.end(),
                                        wavelength);
    return WAVELENGTHS.at[static_cast<std::size_t>(above - WAVELENGTHS.between.begin())];
}

// Sources that step to the same heading and wavelength are one sweep, so
// they are merged; then the weights are taken relative to the strongest, in
// steps, and a source whose weight steps to nothing is dropped.
void settle(std::vector<ShelterSource>& sources) {
    std::sort(sources.begin(), sources.end(), [](const ShelterSource& a, const ShelterSource& b) {
        if (a.travel.x != b.travel.x) return a.travel.x < b.travel.x;
        if (a.travel.y != b.travel.y) return a.travel.y < b.travel.y;
        return a.wavelength < b.wavelength;
    });
    std::vector<ShelterSource> merged;
    for (const ShelterSource& s : sources) {
        if (!merged.empty() && merged.back().travel == s.travel
            && merged.back().wavelength == s.wavelength) {
            merged.back().weight += s.weight;
        } else {
            merged.push_back(s);
        }
    }
    float strongest = 0.0f;
    for (const ShelterSource& s : merged) strongest = std::max(strongest, s.weight);
    sources.clear();
    if (strongest <= 0.0f) return;
    for (ShelterSource s : merged) {
        s.weight = std::round(s.weight / strongest / WEIGHT_STEP) * WEIGHT_STEP;
        if (s.weight > 0.0f) sources.push_back(s);
    }
}

// A field carried down a grid of `n` samples `size` metres across, a row at a
// time in the direction waves of `wavelength` travel, into `field`.
//
// The sweep runs a row at a time along whichever axis the waves travel more
// along, so each row reads only the one before it: a sample takes what was
// one row upwind, a fraction of a cell to the side when the waves cross the
// axis at an angle. It spreads sideways as it goes — read from either side of
// the upwind point, a lateral variance of twice wavelength / 4pi added per
// metre travelled — which is how waves bend around the end of whatever stood
// in their way, its edge widening as the square root of wavelength times
// distance. Outside the grid, and at its upwind edge, the field is `enter`.
// `at(sample, carried, step)` makes a sample's value from what it received
// from upwind, `step` metres back.
template <typename At>
void carry(std::uint32_t n, float size, glm::vec2 direction, float wavelength, float enter,
           std::vector<float>& field, At&& at) {
    const float cell = size / static_cast<float>(n - 1);
    const glm::vec2 travel = glm::normalize(direction);

    const bool along_z = std::abs(travel.y) >= std::abs(travel.x);
    const float major = along_z ? travel.y : travel.x;
    const float minor = along_z ? travel.x : travel.y;
    const float shift = minor / std::abs(major);    // cells to the side per row
    const float step = cell / std::abs(major);      // metres travelled per row
    const float spreading = wavelength / (2.0f * TAU);
    const float sigma = std::sqrt(2.0f * spreading * step);
    const float offset = sigma / (cell * std::abs(major));   // in cells along a row

    const auto index = [&](std::uint32_t column, std::uint32_t row) -> std::size_t {
        const std::uint32_t along = major > 0.0f ? row : n - 1 - row;
        return along_z ? (static_cast<std::size_t>(along) * n) + column
                       : (static_cast<std::size_t>(column) * n) + along;
    };
    const float last = static_cast<float>(n - 1);
    const auto upwind = [&](std::uint32_t row, float column) -> float {
        if (column < 0.0f || column > last) return enter;
        const auto c0 = static_cast<std::uint32_t>(column);
        const std::uint32_t c1 = std::min(c0 + 1, n - 1);
        const float t = column - static_cast<float>(c0);
        const float a = field[index(c0, row)];
        const float b = field[index(c1, row)];
        return a + ((b - a) * t);
    };

    for (std::uint32_t c = 0; c < n; ++c) {
        const std::size_t i = index(c, 0);
        field[i] = at(i, enter, 0.0f);
    }
    for (std::uint32_t r = 1; r < n; ++r) {
        for (std::uint32_t c = 0; c < n; ++c) {
            const float from = static_cast<float>(c) - shift;
            const float carried =
                0.5f * (upwind(r - 1, from - offset) + upwind(r - 1, from + offset));
            const std::size_t i = index(c, r);
            field[i] = at(i, carried, step);
        }
    }
}

// One way a sum arrives, swept down the shelter's grid: the share of its
// energy that reaches each sample, into `energy`. The open sea comes in; land
// keeps nothing.
void sweep(const ShelterLand& land, const ShelterSource& source, std::vector<float>& energy) {
    carry(land.samples, land.size, source.travel, source.wavelength, 1.0f, energy,
          [&](std::size_t i, float carried, float) { return land.land[i] != 0 ? 0.0f : carried; });
}

// Every source of one sum swept and added by weight, into `sum`. Returns the
// total weight.
float sweep_sum(const ShelterLand& land, const std::vector<ShelterSource>& sources,
                std::vector<float>& sum) {
    std::vector<float> energy(sum.size());
    float total = 0.0f;
    for (const ShelterSource& source : sources) {
        if (source.weight <= 0.0f) continue;
        sweep(land, source, energy);
        for (std::size_t i = 0; i < sum.size(); ++i) sum[i] += source.weight * energy[i];
        total += source.weight;
    }
    return total;
}

[[nodiscard]] bool same_grid(const ShoreExposure& exposure, const ShelterLand& land) {
    return exposure.samples == land.samples && exposure.min_corner == land.min_corner
        && exposure.size == land.size;
}

} // namespace

ShelterSources shelter_sources(const FjWaterData& water) {
    ShelterSources out;

    if (water.packed_trains.has_value()) {
        const PackedTrains& t = *water.packed_trains;
        float strongest = 0.0f;
        for (std::size_t i = 0; i < t.count; ++i) {
            const float amplitude = t.amplitude[i];
            if (amplitude <= 0.0f || t.k[i] <= 0.0f) continue;
            const glm::vec2 heading = stepped_heading({t.dir_x[i], t.dir_z[i]});
            const float wavelength = stepped_wavelength(TAU / t.k[i]);
            out.trains.push_back({heading, amplitude * amplitude, wavelength});
            if (amplitude > strongest) {
                strongest = amplitude;
                out.trains_dominant = {heading, TAU / wavelength};
            }
        }
        settle(out.trains);
    }

    if (water.simulated.has_value()) {
        const FftContributor& sea = water.simulated->drawn_from;
        const float k = water.simulated->shoaling.k;
        const float wind = glm::length(sea.wind_direction);
        if (k > 0.0f && wind > 1e-6f && water.simulated->shoaling.amplitude > 0.0f) {
            // Across the spread and out to two and a half times it, where the
            // spreading has fallen to next to nothing, in steps of a quarter of
            // it: a few directions for a narrow swell, more for a storm.
            const float spread = std::max(sea.spread_degrees, 1.0f);
            const float reach = std::min(2.5f * spread, 180.0f);
            const float step = std::max(0.25f * spread, 2.0f);
            const int each_side = static_cast<int>(std::ceil(reach / step));
            const float heading = portable::atan2(sea.wind_direction.y, sea.wind_direction.x);
            const float wavelength = stepped_wavelength(TAU / k);
            out.simulated_dominant = {stepped_heading(sea.wind_direction), TAU / wavelength};
            for (int j = -each_side; j <= each_side; ++j) {
                const float angle =
                    glm::radians(reach * static_cast<float>(j) / static_cast<float>(each_side));
                const float weight = directional_spread(sea, angle);
                if (weight <= 0.0f) continue;
                out.simulated.push_back({stepped_heading(heading_of(heading + angle)), weight,
                                         wavelength});
            }
            settle(out.simulated);
        }
    }
    return out;
}

ShelterLand shelter_land(const ShoreBed& bed) {
    ShelterLand out;
    if (bed.terrain == nullptr) return out;
    const FjTerrainData& terrain = *bed.terrain;
    const std::uint32_t heights = terrain.sample_count();
    const float world = terrain.header.world_size;
    if (heights < 2 || world <= 0.0f
        || terrain.heights.size() < static_cast<std::size_t>(heights) * heights) {
        return out;
    }

    out.margin = MARGIN_SHARE * world;
    out.size = world + (2.0f * out.margin);
    out.samples = std::clamp(static_cast<std::uint32_t>(std::ceil(out.size / SHELTER_SPACING)) + 1,
                             2u, MAX_SHELTER_SAMPLES);
    out.min_corner = bed.min_corner - glm::vec2(out.margin);
    const std::uint32_t n = out.samples;
    const float cell = out.size / static_cast<float>(n - 1);
    out.land.assign(static_cast<std::size_t>(n) * n, 0);

    // Land where the ground stands above the water: a normalized height past
    // this one. A terrain with no height range is land or water throughout.
    const float scale = terrain.header.height_scale;
    const float above = bed.surface_y - bed.ground_y - terrain.header.height_offset;
    const auto is_land = [&](float normalized) {
        return scale > 0.0f ? normalized * scale > above : 0.0f > above;
    };

    // The heights under one cell: every sample from the one at or before its
    // near edge to the one at or after its far edge, so a feature narrower
    // than a cell is still seen.
    const float spacing = world / static_cast<float>(heights - 1);
    const float last = static_cast<float>(heights - 1);
    const auto first_sample = [&](float metres) {
        return static_cast<std::uint32_t>(std::clamp(std::floor(metres / spacing), 0.0f, last));
    };
    const auto last_sample = [&](float metres) {
        return static_cast<std::uint32_t>(std::clamp(std::ceil(metres / spacing), 0.0f, last));
    };

    for (std::uint32_t z = 0; z < n; ++z) {
        const float cz = (static_cast<float>(z) * cell) - out.margin;   // terrain metres
        if (cz + (0.5f * cell) < 0.0f || cz - (0.5f * cell) > world) continue;
        const std::uint32_t z0 = first_sample(cz - (0.5f * cell));
        const std::uint32_t z1 = last_sample(cz + (0.5f * cell));
        for (std::uint32_t x = 0; x < n; ++x) {
            const float cx = (static_cast<float>(x) * cell) - out.margin;
            if (cx + (0.5f * cell) < 0.0f || cx - (0.5f * cell) > world) continue;
            const std::uint32_t x0 = first_sample(cx - (0.5f * cell));
            const std::uint32_t x1 = last_sample(cx + (0.5f * cell));
            float highest = 0.0f;
            for (std::uint32_t hz = z0; hz <= z1; ++hz) {
                const float* row = &terrain.heights[static_cast<std::size_t>(hz) * heights];
                for (std::uint32_t hx = x0; hx <= x1; ++hx) highest = std::max(highest, row[hx]);
            }
            out.land[(static_cast<std::size_t>(z) * n) + x] = is_land(highest) ? 1 : 0;
        }
    }

    // The warp's grid: every other sample of this one, and the still depth at
    // each — the bed as the water reads it, deep where there is none.
    out.warp_samples = ((n - 1) / 2) + 1;
    const std::uint32_t m = out.warp_samples;
    const float warp_cell = out.size / static_cast<float>(m - 1);
    out.warp_depths.assign(static_cast<std::size_t>(m) * m, DEEP);
    for (std::uint32_t z = 0; z < m; ++z) {
        for (std::uint32_t x = 0; x < m; ++x) {
            const glm::vec2 at = out.min_corner + (glm::vec2(x, z) * warp_cell);
            if (const std::optional<StillDepth> still = still_depth(bed, at)) {
                out.warp_depths[(static_cast<std::size_t>(z) * m) + x] = still->depth;
            }
        }
    }
    return out;
}

ShoreExposure bake_exposure(const ShelterLand& land, const ShelterSources& sources,
                            float strength) {
    ShoreExposure out;
    out.samples = land.samples;
    out.min_corner = land.min_corner;
    out.size = land.size;
    const std::uint32_t n = land.samples;
    if (n < 2 || land.land.size() != static_cast<std::size_t>(n) * n) {
        out.samples = 0;
        return out;
    }

    const std::size_t count = static_cast<std::size_t>(n) * n;
    std::vector<float> trains(count, 0.0f);
    std::vector<float> simulated(count, 0.0f);
    const float trains_weight = sweep_sum(land, sources.trains, trains);
    const float simulated_weight = sweep_sum(land, sources.simulated, simulated);

    const float cell = land.size / static_cast<float>(n - 1);
    const auto share = [strength](float energy, float weight) {
        const float baked =
            weight > 0.0f ? std::sqrt(std::clamp(energy / weight, 0.0f, 1.0f)) : 1.0f;
        return 1.0f - (strength * (1.0f - baked));
    };
    const auto stored = [](float value) {
        return static_cast<std::uint16_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 65535.0f));
    };

    out.shares.resize(2 * count);
    for (std::uint32_t z = 0; z < n; ++z) {
        for (std::uint32_t x = 0; x < n; ++x) {
            const std::size_t i = (static_cast<std::size_t>(z) * n) + x;
            // Back to the open sea across the margin, which is what lies past
            // the grid.
            const float kept =
                kept_inside(std::min({x, z, n - 1 - x, n - 1 - z}), cell, land.margin);
            const float g = share(trains[i], trains_weight);
            const float s = share(simulated[i], simulated_weight);
            out.shares[2 * i] = stored(1.0f + ((g - 1.0f) * kept));
            out.shares[(2 * i) + 1] = stored(1.0f + ((s - 1.0f) * kept));
        }
    }
    return out;
}

ShoreWarp bake_warp(const ShelterLand& land, const ShelterSources& sources) {
    ShoreWarp out;
    out.samples = land.warp_samples;
    out.min_corner = land.min_corner;
    out.size = land.size;
    const std::uint32_t m = land.warp_samples;
    const std::size_t count = static_cast<std::size_t>(m) * m;
    if (m < 2 || land.warp_depths.size() != count) {
        out.samples = 0;
        return out;
    }
    out.offsets.assign(4 * count, 0.0f);
    const float cell = land.size / static_cast<float>(m - 1);

    std::vector<float> lag(count);
    std::vector<float> shorter(count);
    const auto bake_one = [&](const WarpSource& dominant, std::uint32_t pair) {
        if (dominant.k <= 0.0f) return;
        for (std::size_t i = 0; i < count; ++i) {
            shorter[i] = shortening_at(dominant.k * land.warp_depths[i]) - 1.0f;
        }
        carry(m, land.size, dominant.travel, TAU / dominant.k, 0.0f, lag,
              [&](std::size_t i, float carried, float step) {
                  return carried + (step * shorter[i]);
              });
        const glm::vec2 travel = glm::normalize(dominant.travel);
        for (std::uint32_t z = 0; z < m; ++z) {
            for (std::uint32_t x = 0; x < m; ++x) {
                const std::size_t i = (static_cast<std::size_t>(z) * m) + x;
                const float kept =
                    kept_inside(std::min({x, z, m - 1 - x, m - 1 - z}), cell, land.margin);
                out.offsets[(4 * i) + (2 * pair)] = travel.x * lag[i] * kept;
                out.offsets[(4 * i) + (2 * pair) + 1] = travel.y * lag[i] * kept;
            }
        }
    };
    bake_one(sources.trains_dominant, 0);
    bake_one(sources.simulated_dominant, 1);
    return out;
}

ShoreDerivation::ShoreDerivation() = default;
ShoreDerivation::ShoreDerivation(ShoreDerivation&&) noexcept = default;
ShoreDerivation& ShoreDerivation::operator=(ShoreDerivation&&) noexcept = default;

ShoreDerivation::~ShoreDerivation() {
    // A worker still baking is finished with before what it bakes for goes
    // away. It holds only shared, unchanging data.
    if (baking_.has_value() && baking_->bakes.valid()) baking_->bakes.wait();
}

void ShoreDerivation::collect() {
    if (!baking_.has_value()) return;
    if (baking_->bakes.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    auto [exposure, warp] = baking_->bakes.get();
    bakes_.exposure = std::make_shared<const ShoreExposure>(std::move(exposure));
    bakes_.warp = std::make_shared<const ShoreWarp>(std::move(warp));
    baked_from_ = std::move(baking_->from);
    baking_.reset();
}

ShoreBakes ShoreDerivation::derive(const ShoreBed& bed, const FjWaterData& water,
                                   ThreadPool* workers, ShoreBakes previous) {
    const LandKey key{bed.terrain.get(), bed.generation, bed.min_corner, bed.ground_y,
                      bed.surface_y};
    if (!land_key_.has_value() || *land_key_ != key) {
        land_ = std::make_shared<const ShelterLand>(shelter_land(bed));
        land_key_ = key;
    }
    collect();

    ShelterSources sources = shelter_sources(water);
    if (sources.trains.empty() && sources.simulated.empty()) return {};   // a flat sea

    if (bakes_.exposure == nullptr && previous.exposure != nullptr
        && same_grid(*previous.exposure, *land_)) {
        // Shown until this derivation's own bake is ready; not taken as
        // baked from anything, so that bake is still asked for.
        bakes_ = std::move(previous);
    }

    const bool current = bakes_.exposure != nullptr && baked_from_.land == land_
                      && baked_from_.sources == sources;
    if (!current) {
        const auto bake = [](const ShelterLand& land, const ShelterSources& baked) {
            return std::make_pair(bake_exposure(land, baked), bake_warp(land, baked));
        };
        if (workers == nullptr) {
            auto [exposure, warp] = bake(*land_, sources);
            bakes_.exposure = std::make_shared<const ShoreExposure>(std::move(exposure));
            bakes_.warp = std::make_shared<const ShoreWarp>(std::move(warp));
            baked_from_ = Baked{land_, std::move(sources)};
        } else if (!baking_.has_value()) {
            // One at a time. While a worker is busy the newest land and sea
            // wait here, and are what gets baked next.
            Baked from{land_, std::move(sources)};
            auto job = workers->submit(
                [land = from.land, baked = from.sources, bake] { return bake(*land, baked); });
            baking_ = Baking{std::move(from), std::move(job)};
        }
    }
    return bakes_;
}

} // namespace fjell::water
