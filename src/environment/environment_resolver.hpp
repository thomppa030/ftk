#pragma once

#include "environment/environment_state.hpp"

namespace fjell {

class Scene;

/// Resolve a scene's environment from its authoring nodes: the first
/// directional light becomes the sun, the world environment node supplies the
/// sky and contact shadows, the volumetric fog node the haze. A scene without
/// a directional light gets a fixed default sun so it still renders lit; a
/// scene without the other nodes gets their defaults with fog disabled.
[[nodiscard]] EnvironmentState resolve_environment(const Scene& scene);

} // namespace fjell
