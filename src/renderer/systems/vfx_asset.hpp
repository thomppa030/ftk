#pragma once

#include <string>

namespace fjell {

struct VfxEffect;

/// Load a .fjvfx v2 file.
[[nodiscard]] VfxEffect load_vfx_effect(const std::string& path);

/// Save a VfxEffect as .fjvfx v2.
void save_vfx_effect(const std::string& path, const VfxEffect& effect);

} // namespace fjell
