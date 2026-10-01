#pragma once

#include "renderer/resources/water_shore.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <utility>
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

/// A sum's dominant wave, which its warp follows: the way it travels and its
/// deep-water wavenumber. No wavenumber, no warp.
struct WarpSource {
    glm::vec2 travel{1.0f, 0.0f};
    float k{0.0f};

    bool operator==(const WarpSource&) const = default;
};

/// Every way each sum of the sea arrives, and each sum's dominant wave.
struct ShelterSources {
    std::vector<ShelterSource> trains;
    std::vector<ShelterSource> simulated;
    WarpSource trains_dominant;
    WarpSource simulated_dominant;

    bool operator==(const ShelterSources&) const = default;
};

/// The ways the sums of a field the world runs on arrive, and their dominant
/// waves: the strongest train, and the simulated sea's peak running downwind.
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
    /// The warp's own grid over the same ground, a sample for every two of
    /// these, and the still-water depth at each of its samples: no more than
    /// nothing on land, and deep off the terrain.
    std::uint32_t warp_samples{0};
    std::vector<float> warp_depths;
};

/// Samples a side, at most: a grid of 2 m over a terrain of up to about 1.3
/// km with its margin, coarser past that.
inline constexpr std::uint32_t MAX_SHELTER_SAMPLES = 1024;

/// Metres between the grid's samples, where the terrain is small enough.
inline constexpr float SHELTER_SPACING = 2.0f;

/// How much of the lee the land casts is kept: 1 the lee as baked, 0 none.
///
/// Half, by choice rather than physics. Baked whole, a cove the land turns
/// from the sea keeps a fifth of the open sea's height or less — calmer still
/// in reality, where a few hundred metres of water raise almost nothing of
/// their own — and reads as dead water in a game. Half keeps every lee
/// clearly calmer than the open sea and the water in it alive.
inline constexpr float SHELTER_STRENGTH = 0.5f;

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
/// the square root of the energy share: the amplitude share. That share keeps
/// `strength` of what the land took (see SHELTER_STRENGTH), and past the
/// terrain it fades to 1 across the margin.
[[nodiscard]] ShoreExposure bake_exposure(const ShelterLand& land, const ShelterSources& sources,
                                          float strength = SHELTER_STRENGTH);

/// The most a wave shortens over the shallows, as k / k_deep: a 40 m wave's
/// 5 m in 10 cm of water. Shallower than that the wave has broken to nothing
/// and its crests would only crowd closer than the water's mesh can draw.
inline constexpr float MAX_SHORTENING = 8.0f;

/// Where each sum is read over `land`'s ground: the lag its dominant wave
/// picks up over the shallows, as an offset along the way it travels.
///
/// A sweep in the direction the wave travels carries the lag: over water of
/// depth d it falls behind the deep sea by `k(d) / k_deep - 1` metres a metre,
/// `k(d)` from the dispersion relation, so in the sea's own coordinates the
/// wave there is that much shorter. Land carries the lag on unchanged. The lag
/// spreads sideways as the shelter's lee does, so where a lee and the open sea
/// meet the crests join rather than step, and past the terrain it fades out
/// across the margin.
[[nodiscard]] ShoreWarp bake_warp(const ShelterLand& land, const ShelterSources& sources);

/// What the shore bakes for a body of water.
struct ShoreBakes {
    std::shared_ptr<const ShoreExposure> exposure;
    std::shared_ptr<const ShoreWarp> warp;
};

/// Keeps one body of water's exposure and warp current as the land and the
/// sea change. The shelter's counterpart to SeaDerivation, and like it needs no
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

    /// The exposure and warp to run on this step for `bed` under `water`'s
    /// sea, or nothing until they have been baked. `previous` is what the
    /// field last had, handed out meanwhile if it was baked on the same grid —
    /// as when a body of water has just been copied into play. With no
    /// `workers` the bake is done here and now.
    [[nodiscard]] ShoreBakes derive(const ShoreBed& bed, const FjWaterData& water,
                                    ThreadPool* workers, ShoreBakes previous = {});

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
        std::future<std::pair<ShoreExposure, ShoreWarp>> bakes;
    };

    /// Take the worker's exposure if it has finished one.
    void collect();

    std::optional<LandKey> land_key_;
    std::shared_ptr<const ShelterLand> land_;
    ShoreBakes bakes_;
    Baked baked_from_;
    std::optional<Baking> baking_;
};

} // namespace water
} // namespace fjell
