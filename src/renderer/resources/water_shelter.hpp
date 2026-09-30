#pragma once

#include "renderer/resources/water_shore.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <vector>

namespace fjell {

struct FjWaterData;
class ThreadPool;

/// Shelter: how much of the sea reaches water that land stands in front of.
///
/// Depth decides how waves shoal and break; it cannot calm a fjord, whose
/// water is deep right up to its walls. What calms it is land between it and
/// the open sea the waves come from. That is a question about the whole
/// terrain rather than a point of it, so it is baked: a sweep across the
/// terrain for each way a sum of the sea arrives, the land in its path
/// casting a lee that fills back in with distance, weighted by how much of
/// the sum arrives that way. The result is an exposure grid (ShoreExposure)
/// both halves of the wave field read per point.
namespace water {

/// One way a sum of the sea arrives: the way its waves travel, how much of
/// the sum's energy travels that way, and the wavelength that sets how fast a
/// lee fills in behind the land it passes.
struct ShelterSource {
    glm::vec2 travel{1.0f, 0.0f};
    float weight{0.0f};
    float wavelength{0.0f};

    bool operator==(const ShelterSource&) const = default;
};

/// Every way each sum of the sea arrives.
struct ShelterSources {
    std::vector<ShelterSource> trains;
    std::vector<ShelterSource> simulated;

    bool operator==(const ShelterSources&) const = default;
};

/// The ways the sums of a field the world runs on arrive.
///
/// The trains each arrive one way, weighted by their energy (amplitude
/// squared). The simulated sea arrives across its whole spread: several
/// directions around its wind, weighted by its own spreading function, since
/// a point hidden from the mean direction still takes the waves that come in
/// from the side.
///
/// In steps — a degree of heading, five percent of wavelength, a percent of
/// weight — so a wind that drifts every frame asks for a new bake when the
/// lee it casts has moved, and not every frame.
[[nodiscard]] ShelterSources shelter_sources(const FjWaterData& water);

/// The grid an exposure is baked on, laid over a bed, and where on it the
/// land stands above the water at rest.
struct ShelterLand {
    std::uint32_t samples{0};      // per side
    glm::vec2 min_corner{0.0f};    // world XZ of sample (0, 0)
    float size{0.0f};              // metres from the first sample to the last
    /// How far past the terrain the grid reaches on each side, in metres:
    /// the width over which the exposure fades back to the open sea.
    float margin{0.0f};
    /// Per sample, row-major: 1 where the ground stands above the water.
    std::vector<std::uint8_t> land;
};

/// Samples a side, at most: a grid of 2 m over a terrain of up to about 1.3
/// km with its margin, coarser past that.
inline constexpr std::uint32_t MAX_SHELTER_SAMPLES = 1024;

/// Metres between the grid's samples, where the terrain is small enough.
inline constexpr float SHELTER_SPACING = 2.0f;

/// Lay the grid over `bed` and mark its land: a sample is land when any of
/// the terrain's heights within its cell stands above the water, so a spit
/// narrower than a cell still blocks. Reads the heights, so it belongs where
/// they are edited: on the thread that edits them, never on a worker.
[[nodiscard]] ShelterLand shelter_land(const ShoreBed& bed);

/// How much of each sum reaches every sample of `land`'s grid.
///
/// For each way a sum arrives, a sweep down the grid in the direction its
/// waves travel carries the share of their energy that got this far: the
/// open sea at the grid's upwind edge, nothing past land. A lee fills back in
/// behind land by spreading sideways as it goes, its edge widening as the
/// square root of wavelength times distance, the way waves bend around the end
/// of a breakwater. The directions are summed by weight, and what is kept is
/// the square root of the energy share: the amplitude share. Past the
/// terrain the share fades to 1 across the margin.
[[nodiscard]] ShoreExposure bake_exposure(const ShelterLand& land, const ShelterSources& sources);

/// Keeps one body of water's exposure current as the land and the sea
/// change. The shelter's counterpart to SeaDerivation, and like it needs no
/// renderer.
///
/// Marking the land is done here, on the calling thread, and again only when
/// the terrain is edited or the water or terrain moves. Baking is done on a
/// worker, one at a time, and the exposure last baked is handed out until the
/// next is ready; the newest sources wait and are what is baked next.
///
/// When a new exposure takes over depends on how quickly a worker baked it,
/// so two machines adopt it a few steps apart, as with a new sea.
class ShoreDerivation {
public:
    ShoreDerivation();
    ~ShoreDerivation();
    ShoreDerivation(ShoreDerivation&&) noexcept;
    ShoreDerivation& operator=(ShoreDerivation&&) noexcept;
    ShoreDerivation(const ShoreDerivation&) = delete;
    ShoreDerivation& operator=(const ShoreDerivation&) = delete;

    /// The exposure to run on this step for `bed` under `water`'s sea, or
    /// nothing until one has been baked. `previous` is what the field last
    /// had, handed out meanwhile if it was baked on the same grid — as when a
    /// body of water has just been copied into play. With no `workers` the
    /// bake is done here and now.
    [[nodiscard]] std::shared_ptr<const ShoreExposure> derive(
        const ShoreBed& bed, const FjWaterData& water, ThreadPool* workers,
        std::shared_ptr<const ShoreExposure> previous = nullptr);

private:
    struct LandKey {
        const FjTerrainData* terrain{nullptr};
        std::uint32_t generation{0};
        glm::vec2 min_corner{0.0f};
        float ground_y{0.0f};
        float surface_y{0.0f};

        bool operator==(const LandKey&) const = default;
    };
    struct Baked {
        std::shared_ptr<const ShelterLand> land;
        ShelterSources sources;
    };
    struct Baking {
        Baked from;
        std::future<ShoreExposure> exposure;
    };

    /// Take the worker's exposure if it has finished one.
    void collect();

    std::optional<LandKey> land_key_;
    std::shared_ptr<const ShelterLand> land_;
    std::shared_ptr<const ShoreExposure> exposure_;
    Baked exposure_from_;
    std::optional<Baking> baking_;
};

} // namespace water
} // namespace fjell
