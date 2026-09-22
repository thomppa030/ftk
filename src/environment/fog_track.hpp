#pragma once

#include "environment/track_set.hpp"

#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fjell {

struct EnvironmentState;
class PipelineCache;
class Scene;

/// How a day profile and a weather event reach the fog. The fog's look is
/// its material, so a track names a uniform of that material:
///
///   fog.<uniform>          the scene's haze, the Volumetric Fog node's material
///   fog.<group>.<uniform>  every Fog Volume whose group is <group>
///
/// `fog.density` thickens the haze by the hour; `fog.valley.noise_contrast`
/// breaks the valley's banks into wisps as the sun comes up. Any uniform the
/// material declares can be driven; a name it does not declare reaches
/// nothing, and the editors say so.
///
/// The four names from before the fog had a material keep working:
/// `fog_density` and `fog_color` are the haze's `density` and `color`, and
/// `fog_density.<group>` and `fog_color.<group>` the group's. A profile
/// written against the old fog node still does what it did.
namespace fog_track {

inline constexpr std::string_view PREFIX = "fog.";
inline constexpr std::string_view DENSITY_UNIFORM = "density";
inline constexpr std::string_view COLOR_UNIFORM = "color";

/// Where a fog track lands: the haze when `group` is empty, else the
/// volumes of that group.
struct Target {
    std::string group;
    std::string uniform;
};

/// The target a track name addresses, or nothing when the name is not a fog
/// track and belongs to the sky-uniform fallthrough instead.
[[nodiscard]] std::optional<Target> parse(std::string_view track_name);

/// The canonical name for a target: `fog.<uniform>` or `fog.<group>.<uniform>`.
[[nodiscard]] std::string name_for(std::string_view group, std::string_view uniform);

/// Fold one track's value into the resolved fog, by the track's mode and
/// faded in by `weight`, the way `fold_track` does for a single value. A
/// uniform the material does not declare is left alone.
void apply(EnvironmentState& state, const Target& target, TrackMode mode, float value,
           float weight = 1.0f);
void apply(EnvironmentState& state, const Target& target, TrackMode mode,
           const glm::vec3& value, float weight = 1.0f);

/// A track the scene's fog can take: one per scalar or colour uniform of the
/// haze's material, and one per uniform of each group's volumes. For an
/// editor's "Add track" list.
struct Preset {
    std::string name;
    bool color{false};
    float range_min{0.0f};
    float range_max{1.0f};
};
[[nodiscard]] std::vector<Preset> presets(const Scene& scene, PipelineCache* cache);

/// Why a fog track would reach nothing in this scene, or empty when it
/// reaches something: no fog node, no volume in the group, or a uniform the
/// material does not declare. For the editors to show beside the track.
[[nodiscard]] std::string problem(std::string_view track_name, const Scene& scene,
                                  PipelineCache* cache);

} // namespace fog_track

} // namespace fjell
