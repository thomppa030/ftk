#pragma once

#include <nlohmann/json_fwd.hpp>

#include <string>

namespace fjell {

class Project;
class ResourceManager;
struct VfxEffect;

/// Load a .fjvfx v2 file.
[[nodiscard]] VfxEffect load_vfx_effect(const std::string& path);

/// Load from pre-parsed JSON (avoids file I/O, used by oneshot pool).
[[nodiscard]] VfxEffect load_vfx_effect_from_json(const nlohmann::json& j);

/// Save a VfxEffect as .fjvfx v2.
void save_vfx_effect(const std::string& path, const VfxEffect& effect);

/// Resolve texture paths in all InitialTextureModules to bindless indices.
void resolve_vfx_textures(VfxEffect& effect, ResourceManager& resources,
                          const Project* project = nullptr);

} // namespace fjell
