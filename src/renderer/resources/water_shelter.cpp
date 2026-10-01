#include "renderer/resources/water_shelter.hpp"

#include "core/thread_pool.hpp"
#include "renderer/resources/fjterrain.hpp"
#include "renderer/resources/fjwater.hpp"
#include "renderer/resources/water_spectrum.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace fjell::water {

namespace {

constexpr float TAU = 6.28318530717958647692f;

// The steps sources are taken in (see shelter_sources()).
constexpr float HEADING_STEP_DEGREES = 1.0f;
constexpr float WAVELENGTH_STEP = 1.05f;
constexpr float WEIGHT_STEP = 0.01f;

// How far past the terrain the grid reaches on each side, as a share of the
// terrain's size: room for a lee cast near the edge to fade out rather than
// stop.
constexpr float MARGIN_SHARE = 0.25f;

[[nodiscard]] glm::vec2 stepped_heading(glm::vec2 travel) {
    const float degrees = glm::degrees(std::atan2(travel.y, travel.x));
    const float stepped =
        glm::radians(std::round(degrees / HEADING_STEP_DEGREES) * HEADING_STEP_DEGREES);
    return {std::cos(stepped), std::sin(stepped)};
}

[[nodiscard]] float stepped_wavelength(float wavelength) {
    const float step = std::log(WAVELENGTH_STEP);
    return std::exp(std::round(std::log(wavelength) / step) * step);
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

// One way a sum arrives, swept down the grid: the share of its energy that
// reaches each sample, into `energy`.
//
// The sweep runs a row at a time along whichever axis the waves travel more
// along, so each row reads only the one before it: a sample takes what was
// one row upwind, a fraction of a cell to the side when the waves cross the
// axis at an angle. Land keeps nothing. The share spreads sideways as it goes
// — read from either side of the upwind point, a lateral variance of twice
// wavelength / 4pi added per metre travelled — which is what fills a lee back
// in behind the end of the land that cast it, its edge widening as the square
// root of wavelength times distance. Outside the grid is open sea.
void sweep(const ShelterLand& land, const ShelterSource& source, std::vector<float>& energy) {
    const std::uint32_t n = land.samples;
    const float cell = land.size / static_cast<float>(n - 1);
    const glm::vec2 travel = glm::normalize(source.travel);

    const bool along_z = std::abs(travel.y) >= std::abs(travel.x);
    const float major = along_z ? travel.y : travel.x;
    const float minor = along_z ? travel.x : travel.y;
    const float shift = minor / std::abs(major);    // cells to the side per row
    const float step = cell / std::abs(major);      // metres travelled per row
    const float spreading = source.wavelength / (2.0f * TAU);
    const float sigma = std::sqrt(2.0f * spreading * step);
    const float offset = sigma / (cell * std::abs(major));   // in cells along a row

    const auto index = [&](std::uint32_t column, std::uint32_t row) -> std::size_t {
        const std::uint32_t along = major > 0.0f ? row : n - 1 - row;
        return along_z ? (static_cast<std::size_t>(along) * n) + column
                       : (static_cast<std::size_t>(column) * n) + along;
    };
    const float last = static_cast<float>(n - 1);
    const auto upwind = [&](std::uint32_t row, float column) -> float {
        if (column < 0.0f || column > last) return 1.0f;
        const auto c0 = static_cast<std::uint32_t>(column);
        const std::uint32_t c1 = std::min(c0 + 1, n - 1);
        const float t = column - static_cast<float>(c0);
        const float a = energy[index(c0, row)];
        const float b = energy[index(c1, row)];
        return a + ((b - a) * t);
    };

    for (std::uint32_t c = 0; c < n; ++c) {
        const std::size_t i = index(c, 0);
        energy[i] = land.land[i] != 0 ? 0.0f : 1.0f;
    }
    for (std::uint32_t r = 1; r < n; ++r) {
        for (std::uint32_t c = 0; c < n; ++c) {
            const std::size_t i = index(c, r);
            if (land.land[i] != 0) {
                energy[i] = 0.0f;
                continue;
            }
            const float from = static_cast<float>(c) - shift;
            energy[i] = 0.5f * (upwind(r - 1, from - offset) + upwind(r - 1, from + offset));
        }
    }
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
        for (std::size_t i = 0; i < t.count; ++i) {
            const float amplitude = t.amplitude[i];
            if (amplitude <= 0.0f || t.k[i] <= 0.0f) continue;
            out.trains.push_back({stepped_heading({t.dir_x[i], t.dir_z[i]}), amplitude * amplitude,
                                  stepped_wavelength(TAU / t.k[i])});
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
            const float heading = std::atan2(sea.wind_direction.y, sea.wind_direction.x);
            const float wavelength = stepped_wavelength(TAU / k);
            for (int j = -each_side; j <= each_side; ++j) {
                const float angle =
                    glm::radians(reach * static_cast<float>(j) / static_cast<float>(each_side));
                const float weight = directional_spread(sea, angle);
                if (weight <= 0.0f) continue;
                const glm::vec2 travel{std::cos(heading + angle), std::sin(heading + angle)};
                out.simulated.push_back({stepped_heading(travel), weight, wavelength});
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
            const std::uint32_t edge = std::min({x, z, n - 1 - x, n - 1 - z});
            const float into = land.margin > 0.0f
                ? std::clamp(static_cast<float>(edge) * cell / land.margin, 0.0f, 1.0f)
                : 1.0f;
            const float kept = into * into * (3.0f - (2.0f * into));
            const float g = share(trains[i], trains_weight);
            const float s = share(simulated[i], simulated_weight);
            out.shares[2 * i] = stored(1.0f + ((g - 1.0f) * kept));
            out.shares[(2 * i) + 1] = stored(1.0f + ((s - 1.0f) * kept));
        }
    }
    return out;
}

ShoreDerivation::ShoreDerivation() = default;
ShoreDerivation::ShoreDerivation(ShoreDerivation&&) noexcept = default;
ShoreDerivation& ShoreDerivation::operator=(ShoreDerivation&&) noexcept = default;

ShoreDerivation::~ShoreDerivation() {
    // A worker still baking is finished with before what it bakes for goes
    // away. It holds only shared, unchanging data.
    if (baking_.has_value() && baking_->exposure.valid()) baking_->exposure.wait();
}

void ShoreDerivation::collect() {
    if (!baking_.has_value()) return;
    if (baking_->exposure.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    exposure_ = std::make_shared<const ShoreExposure>(baking_->exposure.get());
    exposure_from_ = std::move(baking_->from);
    baking_.reset();
}

std::shared_ptr<const ShoreExposure> ShoreDerivation::derive(
    const ShoreBed& bed, const FjWaterData& water, ThreadPool* workers,
    std::shared_ptr<const ShoreExposure> previous) {
    const LandKey key{bed.terrain.get(), bed.generation, bed.min_corner, bed.ground_y,
                      bed.surface_y};
    if (!land_key_.has_value() || *land_key_ != key) {
        land_ = std::make_shared<const ShelterLand>(shelter_land(bed));
        land_key_ = key;
    }
    collect();

    ShelterSources sources = shelter_sources(water);
    if (sources.trains.empty() && sources.simulated.empty()) return nullptr;   // a flat sea

    if (exposure_ == nullptr && previous != nullptr && same_grid(*previous, *land_)) {
        // Shown until this derivation's own bake is ready; not taken as
        // baked from anything, so that bake is still asked for.
        exposure_ = std::move(previous);
    }

    const bool current = exposure_ != nullptr && exposure_from_.land == land_
                      && exposure_from_.sources == sources;
    if (!current) {
        if (workers == nullptr) {
            exposure_ = std::make_shared<const ShoreExposure>(bake_exposure(*land_, sources));
            exposure_from_ = Baked{land_, std::move(sources)};
        } else if (!baking_.has_value()) {
            // One at a time. While a worker is busy the newest land and sea
            // wait here, and are what gets baked next.
            Baked from{land_, std::move(sources)};
            auto job = workers->submit([land = from.land, baked = from.sources] {
                return bake_exposure(*land, baked);
            });
            baking_ = Baking{std::move(from), std::move(job)};
        }
    }
    return exposure_;
}

} // namespace fjell::water
