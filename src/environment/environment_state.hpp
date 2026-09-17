#pragma once

#include "environment/sun_path.hpp"
#include "environment/weather_asset.hpp"
#include "physics/physics_types.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace fjell {

// ── Resolved environment ─────────────────────────────────────────────────
//
// Everything the renderer needs to know about the world's lighting and
// atmosphere for one frame, resolved once from the scene's authoring nodes
// (directional light, world environment, volumetric fog, fog volumes). Passes
// read this struct and never the nodes: the value a pass sees is the finished
// one, after every rule that shapes it has run, and a new rule is added in one
// place.
//
// Colours are linear light, ready to multiply. Directions toward a body are
// normalized and point from the surface to the sun or moon.

/// A named value bound for a sky shader uniform: one float, or a colour in
/// sRGB as the shader's hint_color expects it, up to four components.
struct NamedValue {
    std::string name;
    uint32_t count{1};
    std::array<float, 4> values{};

    bool operator==(const NamedValue&) const = default;
};

/// A distant light source: the sun, or the moon.
struct DirectionalBody {
    glm::vec3 direction{0.0f, 1.0f, 0.0f};  // toward the body
    glm::vec3 color{1.0f};
    float intensity{0.0f};
    /// Angle above the horizon in radians, negative once the body has set.
    float elevation{0.0f};

    [[nodiscard]] bool lit() const { return intensity > 0.0f; }
};

/// The world environment node's sky settings.
struct SkySettings {
    glm::vec3 top_color{0.15f, 0.25f, 0.45f};
    glm::vec3 bottom_color{0.8f, 0.4f, 0.1f};
    float ibl_intensity{1.0f};
    std::string hdri_path;
    std::string sky_shader;

    bool atmosphere_enabled{false};
    float atmosphere_rayleigh_scale{8500.0f};
    float atmosphere_mie_scale{1200.0f};
    float atmosphere_mie_g{0.76f};
    float atmosphere_mie_coeff{21e-6f};
    float atmosphere_sun_intensity{22.0f};
    float atmosphere_hdri_floor{0.1f};
};

struct ContactShadowSettings {
    bool enabled{true};
    float distance{0.3f};
    float thickness{0.05f};
    int steps{12};
};

/// The volumetric fog node's haze. `enabled` is false when the scene has none.
struct FogSettings {
    bool enabled{false};
    float density{0.003f};
    float height{0.0f};          // world Y the height falloff starts from
    float height_falloff{0.5f};
    float scattering{0.5f};
    float anisotropy{0.7f};
    float max_distance{130.0f};
    float temporal_blend{0.9f};
    glm::vec3 color{0.8f, 0.85f, 0.9f};
};

/// Something moving through the world that anything reacting to movement can
/// read: fog stirred by a runner, grass bent aside, a track in the snow.
/// Gathered from the scene so the renderer never reaches into physics.
struct MotionSource {
    glm::vec3 position{0.0f};
    /// How fast and which way, in metres per second. The direction is what
    /// makes a wake read as displaced air rather than as a hole.
    glm::vec3 velocity{0.0f};
    /// How wide a mark it leaves, in metres.
    float radius{0.5f};
    /// How far above `position` it reaches, so a consumer filling a volume can
    /// leave the air above a walker's head alone.
    float height{1.8f};
};

/// One placed fog volume, resolved from a FogVolumeNode. The transform is the
/// world matrix as authored; the renderer inverts it on the way to the GPU.
struct FogVolume {
    glm::mat4 transform{1.0f};
    glm::vec3 color{0.8f, 0.85f, 0.9f};
    float density{0.05f};
    float scattering_mult{1.0f};
    float edge_falloff{0.2f};
    ColliderShape shape{ColliderShape::Box};
    /// The volume's group, for the `fog_density.<group>` tracks a day profile
    /// and a weather event scale it by. Empty means neither reaches it.
    std::string group;

    float noise_contrast{0.0f};
    float noise_scale{8.0f};
    /// Where the field has drifted to, in world metres. Already integrated by
    /// the node, so everything downstream only copies it.
    glm::vec3 noise_drift{0.0f};
    int noise_octaves{2};
};

/// A day profile and a weather event both reach fog volumes by the same
/// naming convention: a track called `fog_density.valley` scales the density
/// of every volume whose group is `valley`, and `fog_color.valley` sets their
/// colour. The ungrouped `fog_density` and `fog_color` keep meaning the global
/// fog alone, so a profile written before volumes existed still does what it
/// always did.
namespace fog_group_track {

/// The group a track name addresses, or empty when it addresses none. The
/// prefix must be `fog_density.` or `fog_color.`, which is what keeps these
/// out of the sky-uniform fallthrough that takes every other unknown name.
[[nodiscard]] std::string_view group_of(std::string_view track_name,
                                        std::string_view prefix);

inline constexpr std::string_view DENSITY_PREFIX = "fog_density.";
inline constexpr std::string_view COLOR_PREFIX = "fog_color.";

} // namespace fog_group_track

struct SunShaftSettings {
    bool enabled{false};
    float intensity{1.0f};
    float density_mult{1.0f};
    float max_reach{200.0f};
    int step_count{32};
    float anisotropy{0.75f};
    float directionality{1.0f};
    float threshold{0.0f};
    float halo_inner{0.95f};
    float halo_outer{0.6f};
};

/// The weather as composited from the playing events: what the sky, the
/// surfaces, the particles and the scripts read this frame.
struct WeatherParams {
    float cloud_coverage{0.0f};
    float cloud_density{0.5f};
    float precipitation{0.0f};
    PrecipitationType precipitation_type{PrecipitationType::None};

    glm::vec3 wind_direction{0.0f, 0.0f, 1.0f};   // the way the wind blows
    float wind_speed{0.0f};                       // m/s before the gust
    float wind_gust{1.0f};                        // multiplier on the speed this frame
    /// How far the wind has carried things so far, in metres on the ground
    /// plane: the integral of the wind, so clouds and anything else that
    /// rides it move smoothly through the gusts instead of sloshing.
    glm::vec2 wind_drift{0.0f};

    /// Integrated over time: how wet and how snow-covered exposed surfaces are.
    float wetness{0.0f};
    float snow{0.0f};

    float lightning{0.0f};        // flash ramp, 0..1
    float lightning_rate{0.0f};   // strikes per minute

    /// The front's travel direction on the ground plane, its edge across the
    /// sky (-1 windward horizon .. 1 leeward) and how soft that edge is.
    glm::vec2 front_direction{0.0f, 1.0f};
    float front_edge{-1.35f};
    float front_softness{0.35f};

    glm::vec3 sky_tint{1.0f};

    /// The active event's id (a resource handle id, 0 when none), phase and
    /// progress through it, for scripts.
    uint32_t event_id{0};
    WeatherPhase phase{WeatherPhase::Sustain};
    float phase_progress{1.0f};
    /// The events' custom tracks by name.
    std::vector<NamedValue> params;
};

struct EnvironmentState {
    /// The hour of the day, 0..24, and the part of the day it falls in. A
    /// scene without a time of day sits at its authored noon.
    float hour{12.0f};
    TimePhase phase{TimePhase::Day};

    DirectionalBody sun;
    DirectionalBody moon;
    /// 0..1 through the lunar month: new at 0 and 1, full at 0.5.
    float moon_phase{0.5f};
    /// The axis the sky turns about and how far it has turned since noon,
    /// for anything fixed to the celestial sphere.
    glm::vec3 celestial_axis{0.0f, 0.0f, 1.0f};
    float celestial_angle{0.0f};

    /// The body that casts the cascaded shadows and fills the global UBO's
    /// directional light this frame.
    DirectionalBody primary;
    /// The other body, lit without cascades.
    DirectionalBody secondary;

    glm::vec3 ambient_color{0.1f, 0.1f, 0.12f};
    float ambient_intensity{0.4f};

    /// EV added on top of the project's exposure, from the day profile.
    float exposure_bias{0.0f};
    /// How bright the sky's star field is, 0 by day.
    float star_intensity{0.0f};
    /// Values for the sky shader's uniforms, by name: the environment node's
    /// overrides and the day profile's custom tracks.
    std::vector<NamedValue> sky_uniforms;

    SkySettings sky;
    ContactShadowSettings contact_shadows;
    FogSettings fog;
    /// Every fog volume in the scene, in scene order. They add, so the order
    /// is not meaningful and nothing sorts them.
    std::vector<FogVolume> fog_volumes;
    /// Everything moving through the world this frame, for the motion field.
    /// Empty when nothing moves, which is what lets the pass skip its work.
    std::vector<MotionSource> motion_sources;
    SunShaftSettings sun_shafts;

    WeatherParams weather;
};

} // namespace fjell
