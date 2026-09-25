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

/// How a day profile and a weather event reach a node whose look is a
/// material. A track names a uniform of that material, and its prefix says
/// which nodes carry it:
///
///   fog.<uniform>           the scene's haze, the Volumetric Fog node's material
///   fog.<group>.<uniform>   every Fog Volume whose group is <group>
///   post.<uniform>          every Post Process Volume with infinite bounds
///   post.<group>.<uniform>  every Post Process Volume whose group is <group>
///
/// `fog.density` thickens the haze by the hour; `fog.valley.noise_contrast`
/// breaks the valley's banks into wisps as the sun comes up; `post.saturation`
/// drains the scene's colour at dusk. Any uniform the material declares can
/// be driven; a name it does not declare reaches nothing, and the editors say
/// so. A name with neither prefix is none of these, and falls through to the
/// sky uniforms.
///
/// The four fog names from before the fog had a material keep working:
/// `fog_density` and `fog_color` are the haze's `density` and `color`, and
/// `fog_density.<group>` and `fog_color.<group>` the group's. A profile
/// written against the old fog node still does what it did.
namespace material_track {

/// The fog's density and colour, which the legacy names drive and a few
/// readers ask for by name.
inline constexpr std::string_view FOG_DENSITY_UNIFORM = "density";
inline constexpr std::string_view FOG_COLOR_UNIFORM = "color";

/// Which kind of node a track reaches.
enum class Kind : uint8_t { Fog, Post };

/// Where a track lands: with no group, the kind's scene-wide node (the haze,
/// or the infinite post volumes); with one, that kind's volumes of the group.
struct Target {
    Kind kind{Kind::Fog};
    std::string group;
    std::string uniform;
};

/// The target a track name addresses, or nothing when the name is not a
/// material track and belongs to the sky-uniform fallthrough instead.
[[nodiscard]] std::optional<Target> parse(std::string_view track_name);

/// The canonical name for a target: `<prefix>.<uniform>` or
/// `<prefix>.<group>.<uniform>`.
[[nodiscard]] std::string name_for(Kind kind, std::string_view group, std::string_view uniform);

/// Fold one track's value into the resolved state, by the track's mode and
/// faded in by `weight`, the way `fold_track` does for a single value. A
/// uniform the material does not declare is left alone.
void apply(EnvironmentState& state, const Target& target, TrackMode mode, float value,
           float weight = 1.0f);
void apply(EnvironmentState& state, const Target& target, TrackMode mode,
           const glm::vec3& value, float weight = 1.0f);

/// A track the scene's materials can take: one per scalar or colour uniform
/// of the haze and the infinite post volumes, and one per uniform of each
/// group's volumes. For an editor's "Add track" list, with the heading it is
/// listed under and the mode and range it starts with.
struct Preset {
    std::string name;
    std::string heading;
    bool color{false};
    TrackMode mode{TrackMode::Override};
    float range_min{0.0f};
    float range_max{1.0f};
};
[[nodiscard]] std::vector<Preset> presets(const Scene& scene, PipelineCache* cache);

/// The heading a track is listed under in an editor: "Fog", "Post", or the
/// kind and its group.
[[nodiscard]] std::string heading(const Target& target);

/// Why a material track would reach nothing in this scene, or empty when it
/// reaches something: no node of the kind, no volume in the group, or a
/// uniform the material does not declare. For the editors to show beside the
/// track.
[[nodiscard]] std::string problem(std::string_view track_name, const Scene& scene,
                                  PipelineCache* cache);

} // namespace material_track

} // namespace fjell
