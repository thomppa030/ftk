#pragma once

#include "renderer/resources/water_shelter.hpp"

#include <vector>

namespace fjell::water {

/// Refraction: how the dominant wave's crests run over the ground.
///
/// A wave slows as the water shallows, and the part of a crest in shallower
/// water lags the rest, so the crest turns to face the shallows: waves come in
/// to a beach nearly square to it whatever way they arrive offshore, and wrap
/// round a headland into the bay behind it. Where a wave is, is a question of
/// the whole path it took, so it is solved over the shore once, on the warp's
/// grid, and baked into where each sum of the sea is read.

/// How many times shorter a wave is in water `mu` = k_deep * d deep than in
/// deep water, k / k_deep from the dispersion relation, held to
/// MAX_SHORTENING. 1 on land, and from SHOALING_DEEP_MU down.
[[nodiscard]] float shortening_at(float mu);

/// When `dominant`'s waves reach each sample of `land`'s warp grid, as how far
/// the deep sea has come along the way they travel by then: the phase they
/// arrive in, over their deep wavenumber, in metres. Over open sea that is
/// just how far along the way of travel the sample lies; over the shallows
/// the waves arrive later, and behind land by the way round it.
///
/// The first front to arrive, by fast sweeping over the grid: the sea enters
/// as the plane wave it is offshore wherever the grid's edge faces it, and
/// land stands in its way, a sample or the ground between two of them. Row
/// major, as the grid's depths; infinite on land and in water no front
/// reaches.
[[nodiscard]] std::vector<double> arrival(const ShelterLand& land, const WarpSource& dominant);

} // namespace fjell::water
