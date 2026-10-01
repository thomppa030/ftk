#pragma once

#include "ftk/base/result.hpp"
#include "renderer/resources/fjterrain.hpp"

#include <string>

namespace fjell {

/// Read a .fjterrain from disk. Fails on a bad magic, an unsupported version,
/// or a truncated file.
[[nodiscard]] ftk::Result<FjTerrainData> load_fjterrain(const std::string& path);

/// Write a .fjterrain. `data.header` is rewritten from the actual array sizes,
/// so callers need only set the descriptive fields (world size, height scale,
/// patch quads, layers).
[[nodiscard]] ftk::Result<> save_fjterrain(const std::string& path, const FjTerrainData& data);

} // namespace fjell
