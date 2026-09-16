#pragma once

#include "environment/environment_state.hpp"

namespace fjell {

class Scene;
struct WorldEnvironmentNode;

/// The scene's world environment node, or null when it has none. The first
/// one found is the scene's; a second is ignored, as the renderer ignores it.
[[nodiscard]] WorldEnvironmentNode* find_world_environment(Scene& scene);
[[nodiscard]] const WorldEnvironmentNode* find_world_environment(const Scene& scene);

/// Whether the moon should cast the shadows given how bright each body is
/// above the horizon. `moon_led` is the previous frame's answer: the leader
/// only changes hands when the other body is clearly brighter, so twilight
/// does not flip the shadow caster back and forth.
[[nodiscard]] bool moon_leads(const DirectionalBody& sun, const DirectionalBody& moon,
                              bool moon_led);

/// Resolve a scene's environment from its authoring nodes: the first
/// directional light becomes the sun, the world environment node supplies the
/// sky, contact shadows and time of day, the volumetric fog node the haze.
/// With a time of day enabled the sun's direction comes from the sun path at
/// the node's hour and its intensity fades out across the horizon. A scene
/// without a directional light gets a fixed default sun so it still renders
/// lit; a scene without the other nodes gets their defaults with fog disabled.
/// `moon_led` carries the shadow-caster choice across frames, see moon_leads().
[[nodiscard]] EnvironmentState resolve_environment(const Scene& scene, bool moon_led = false);

} // namespace fjell
