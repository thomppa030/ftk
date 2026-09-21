#pragma once

#include "renderer/resources/fjwater.hpp"
#include "renderer/resources/water_spectrum.hpp"

#include <glm/glm.hpp>

#include <future>
#include <memory>
#include <optional>

namespace fjell {

class ThreadPool;

namespace water {

/// Derives, frame after frame, the field one body of water runs on: the
/// authored field as the wind has made it, with its simulated contributor
/// drawn.
///
/// The state behind `derive_effective()`. Drawing a sea takes milliseconds and
/// a sea under the weather becomes a different one every time the wind crosses
/// a step, so this keeps the sea it last drew, shapes the next one on a worker
/// and goes on handing out the last until that is done. The renderer and
/// physics are both given the result, in the same frame, so the sea a boat
/// floats on changes when the sea that is drawn does.
///
/// It needs no renderer: a server with no GPU derives the same field.
///
/// When a new sea takes over depends on how quickly a worker shaped it, so two
/// machines blown by the same wind adopt it a few frames apart. Closing that
/// wants the simulation to run on a fixed step first, so that "a few frames"
/// means the same thing everywhere.
class SeaDerivation {
public:
    SeaDerivation();
    ~SeaDerivation();
    SeaDerivation(SeaDerivation&&) noexcept;
    SeaDerivation& operator=(SeaDerivation&&) noexcept;
    SeaDerivation(const SeaDerivation&) = delete;
    SeaDerivation& operator=(const SeaDerivation&) = delete;

    /// The field to run on this frame. `wind_xz` as `apply_wind()` takes it.
    /// With no `workers`, a new sea is drawn here and now.
    [[nodiscard]] FjWaterData derive(const FjWaterData& authored, glm::vec2 wind_xz,
                                     ThreadPool* workers);

private:
    /// Take the worker's sea if it has finished one that still fits.
    void collect(const FftContributor& wanted);

    /// The random part of the sea, which depends on the seed and resolution
    /// alone and is most of what a draw costs. Shared with the worker.
    std::shared_ptr<const SpectrumNoise> noise_;
    std::optional<FjWaterData::SimulatedSea> sea_;

    struct Shaping {
        FftContributor from;
        std::future<FjWaterData::SimulatedSea> sea;
    };
    std::optional<Shaping> shaping_;
};

} // namespace water
} // namespace fjell
