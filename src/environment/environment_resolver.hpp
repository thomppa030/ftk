#pragma once

#include "environment/environment_state.hpp"

namespace fjell {

class Scene;
struct WorldEnvironmentNode;
struct VolumetricFogNode;
struct WeatherSkyOverride;

/// The scene's world environment node, or null when it has none. The first
/// one found is the scene's; a second is ignored, as the renderer ignores it.
[[nodiscard]] WorldEnvironmentNode* find_world_environment(Scene& scene);
[[nodiscard]] const WorldEnvironmentNode* find_world_environment(const Scene& scene);

/// The scene's Volumetric Fog node, the first one when there are several,
/// which is the one the resolver reads. Null when the scene has none.
[[nodiscard]] VolumetricFogNode* find_volumetric_fog(Scene& scene);

/// Whether the moon should cast the shadows given how bright each body is
/// above the horizon. `moon_led` is the previous frame's answer: the leader
/// only changes hands when the other body is clearly brighter, so twilight
/// does not flip the shadow caster back and forth.
[[nodiscard]] bool moon_leads(const DirectionalBody& sun, const DirectionalBody& moon,
                              bool moon_led);

/// How clearly the leading body outshines the other, from 0 to 1: zero while
/// the two are close enough to trade places, one once the leader is several
/// times brighter or the other gives no light at all.
///
/// The trade itself is a cut — direction, colour and cascades all change in one
/// frame — and it has to be, since only one body can cast the shadows. What is
/// drawn from the leader's direction alone can fade by this instead, and be at
/// nothing on both sides of the cut.
[[nodiscard]] float primary_lead(const DirectionalBody& primary,
                                 const DirectionalBody& secondary);

/// Resolve a scene's environment from its authoring nodes: the first
/// directional light becomes the sun, the world environment node supplies the
/// sky, contact shadows and time of day, the volumetric fog node the haze.
/// With a time of day enabled the sun's direction comes from the sun path at
/// the node's hour and its intensity fades out across the horizon. A scene
/// without a directional light gets a fixed default sun so it still renders
/// lit; a scene without the other nodes gets their defaults with fog disabled.
/// `moon_led` carries the shadow-caster choice across frames, see moon_leads().
/// `weather_sky_override` is the day profile and sky shader the active
/// weather brings, faded in by its weight: the scene's profile is blended
/// toward it, and past half weight the event's sky shader and custom tracks
/// take over.
/// `dt` is how long the frame took, for measuring what a node without a
/// stated velocity did by where it was. Zero leaves those sources still,
/// which is what a caller resolving outside the frame loop wants.
[[nodiscard]] EnvironmentState resolve_environment(const Scene& scene, bool moon_led = false,
                                                   const WeatherSkyOverride* weather_sky_override = nullptr,
                                                   float dt = 0.0f);

} // namespace fjell
