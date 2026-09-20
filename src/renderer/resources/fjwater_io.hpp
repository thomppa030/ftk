#pragma once

#include "core/result.hpp"
#include "renderer/resources/fjwater.hpp"

#include <string>

namespace fjell {

/// Read a `.fjwater`. Fails on a missing or malformed file, and on a
/// contributor of a kind this build does not know.
///
/// A file written before contributors existed reads as one Gerstner
/// contributor, at the surface it was tuned to. Within a contributor, trains
/// past MAX_WATER_WAVES are dropped, and a train with a zero direction or a
/// non-positive wavelength is dropped too rather than silently contributing
/// nothing. Gerstner contributors past MAX_GERSTNER_CONTRIBUTORS are dropped.
[[nodiscard]] Result<FjWaterData> load_fjwater_file(const std::string& path);

/// Write a `.fjwater`, always in the current format. Values at their defaults are still written: this is a
/// file an author reads and edits, and a wave list with holes in it is harder
/// to follow than one that spells every train out.
[[nodiscard]] Result<> save_fjwater_file(const std::string& path, const FjWaterData& water);

} // namespace fjell
