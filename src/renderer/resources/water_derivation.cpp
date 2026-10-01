#include "renderer/resources/water_derivation.hpp"

#include "ftk/base/thread_pool.hpp"
#include "renderer/resources/water_surface.hpp"

#include <bit>
#include <chrono>

namespace fjell::water {

namespace {

// The grid a sea is simulated on. A sea on another grid shares nothing with
// this one that could go on being shown while the new one is shaped: the
// textures tile at a different size or hold a different number of samples.
[[nodiscard]] bool same_grid(const FftContributor& a, const FftContributor& b) {
    return a.patch_size == b.patch_size && a.seed == b.seed
        && std::bit_floor(a.resolution) == std::bit_floor(b.resolution);
}

} // namespace

SeaDerivation::SeaDerivation() = default;
SeaDerivation::SeaDerivation(SeaDerivation&&) noexcept = default;
SeaDerivation& SeaDerivation::operator=(SeaDerivation&&) noexcept = default;

SeaDerivation::~SeaDerivation() {
    // A worker still shaping a sea is finished with before what it was shaping
    // it for goes away. It holds only copies and shared, unchanging data.
    if (shaping_.has_value() && shaping_->sea.valid()) shaping_->sea.wait();
}

void SeaDerivation::collect(const FftContributor& wanted) {
    if (!shaping_.has_value()) return;
    if (shaping_->sea.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;

    FjWaterData::SimulatedSea shaped = shaping_->sea.get();
    const FftContributor from = shaping_->from;
    shaping_.reset();
    // Dropped if the grid has changed under it: that change drew its own sea.
    if (same_grid(from, wanted)) sea_ = std::move(shaped);
}

FjWaterData SeaDerivation::derive(const FjWaterData& authored, glm::vec2 wind_xz,
                                  ftk::ThreadPool* workers) {
    FjWaterData out = apply_wind(authored, wind_xz);
    const FftContributor* wanted = simulated_sea(out);
    if (wanted == nullptr || wanted->resolution < 2 || wanted->patch_size <= 0.0f) {
        sea_.reset();
        return out;
    }
    collect(*wanted);

    const std::uint32_t resolution = std::bit_floor(wanted->resolution);
    if (noise_ == nullptr || noise_->seed != wanted->seed || noise_->resolution != resolution) {
        noise_ = std::make_shared<const SpectrumNoise>(draw_noise(wanted->seed, resolution));
    }

    const bool drawn = sea_.has_value()
        && spectrum_settings(sea_->drawn_from) == spectrum_settings(*wanted);
    if (!drawn) {
        if (sea_.has_value() && same_grid(sea_->drawn_from, *wanted) && workers != nullptr) {
            // One at a time. While a worker is busy the newest settings wait in
            // the authored field, and are what gets shaped next.
            if (!shaping_.has_value()) {
                shaping_ = Shaping{
                    *wanted, workers->submit([settings = *wanted, noise = noise_] {
                        return draw_simulated_sea(settings, noise.get());
                    })};
            }
        } else {
            // A different grid, or the first sea there has been: nothing drawn
            // so far stands for it, so there is nothing to show meanwhile.
            shaping_.reset();
            sea_ = draw_simulated_sea(*wanted, noise_.get());
        }
    }

    // The pinch and the count of waves physics feels follow at once, whichever
    // sea is showing: neither waits on a redraw.
    retune(*sea_, *wanted);
    out.simulated = sea_;
    return out;
}

} // namespace fjell::water
