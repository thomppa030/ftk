#pragma once

#include "core/result.hpp"
#include "renderer/resources/fjwater.hpp"

#include <string>

namespace fjell {

/// Read a `.fjwater`. Trains past MAX_WATER_WAVES are dropped, and a train
/// with a zero direction or a non-positive wavelength is dropped too rather
/// than silently contributing nothing.
[[nodiscard]] Result<FjWaterData> load_fjwater_file(const std::string& path);

/// Write a `.fjwater`. Values at their defaults are still written: this is a
/// file an author reads and edits, and a wave list with holes in it is harder
/// to follow than one that spells every train out.
[[nodiscard]] Result<> save_fjwater_file(const std::string& path, const FjWaterData& water);

} // namespace fjell
