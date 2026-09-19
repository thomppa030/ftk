#pragma once

#include "physics/physics_types.hpp"
#include "renderer/resources/resource_handle.hpp"
#include "scene/scene.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace fjell {

struct DayProfileAsset;
struct ClimateAsset;
struct WeatherAsset;

// The same guard weather_asset.hpp uses, so the enum has one definition
// whichever header a translation unit reaches first.
#ifndef FJELL_WEATHER_PHASE_DEFINED
#define FJELL_WEATHER_PHASE_DEFINED
enum class WeatherPhase : int { Approach = 0, Onset = 1, Sustain = 2, Tail = 3 };
#endif

// ── Light nodes ─────────────────────────────────────────────────────────
// Every colour below is linear light, ready for the renderer to multiply.
// The scene file and the inspector hold sRGB and convert on the way in and
// out (node_serializer.cpp, color_edit_linear), so a default written here
// is the value the shaders see.

enum class LightType : uint8_t { Directional, Point, Spot };

struct DirectionalLightNode : Node3D {
    glm::vec3 direction{-0.3f, -1.0f, -0.5f};
    glm::vec3 color{1.0f, 1.0f, 0.95f};
    float intensity{1.5f};
    glm::vec3 ambient_color{0.1f, 0.1f, 0.12f};
    float ambient_intensity{1.0f};

    [[nodiscard]] std::unique_ptr<Node> clone() const override {
        auto copy = std::make_unique<DirectionalLightNode>();
        clone_3d_into(*copy);
        copy->direction = direction;
        copy->color = color;
        copy->intensity = intensity;
        copy->ambient_color = ambient_color;
        copy->ambient_intensity = ambient_intensity;
        return copy;
    }
};

struct PointLightNode : Node3D {
    glm::vec3 color{1.0f};
    float intensity{1.0f};
    float radius{10.0f};
    float vol_scatter_intensity{1.0f};
    bool cast_shadows{false};
    float shadow_bias{0.005f};

    [[nodiscard]] std::unique_ptr<Node> clone() const override {
        auto copy = std::make_unique<PointLightNode>();
        clone_3d_into(*copy);
        copy->color = color;
        copy->intensity = intensity;
        copy->radius = radius;
        copy->vol_scatter_intensity = vol_scatter_intensity;
        copy->cast_shadows = cast_shadows;
        copy->shadow_bias = shadow_bias;
        return copy;
    }
};

struct SpotLightNode : Node3D {
    glm::vec3 direction{0.0f, -1.0f, 0.0f};
    glm::vec3 color{1.0f};
    float intensity{1.0f};
    float radius{10.0f};
    float inner_angle{20.0f};
    float outer_angle{30.0f};
    float vol_scatter_intensity{1.0f};
    bool cast_shadows{false};
    float shadow_bias{0.002f};

    [[nodiscard]] std::unique_ptr<Node> clone() const override {
        auto copy = std::make_unique<SpotLightNode>();
        clone_3d_into(*copy);
        copy->direction = direction;
        copy->color = color;
        copy->intensity = intensity;
        copy->radius = radius;
        copy->inner_angle = inner_angle;
        copy->outer_angle = outer_angle;
        copy->vol_scatter_intensity = vol_scatter_intensity;
        copy->cast_shadows = cast_shadows;
        copy->shadow_bias = shadow_bias;
        return copy;
    }
};

// ── Volumes ──────────────────────────────────────────────────────────────

// A region of the world, placed by its transform: the unit box or the unit
// sphere in the node's own space, so position, rotation and extent all come
// from the transform and there is no separate size to keep in step with it.
//
// What a volume *does* is the derived node's business — fog adds density,
// others will override render settings. What they share is the region and how
// far inside it a point sits, which is `volume_occupancy()` in
// shaders/include/fjell_volume.glsl. A CPU mirror of that function belongs
// here when a volume needs answering on the CPU; fog does not.
struct VolumeNode : Node3D {
    /// Box and Sphere are the analytic shapes a volume can be; Capsule and
    /// Mesh are not implemented and read as Box.
    ColliderShape shape{ColliderShape::Box};
    /// Fraction of the extent the edge fades across, so scaling a volume
    /// keeps its edge proportionally soft. 0 is a hard edge.
    float edge_falloff{0.2f};

protected:
    void clone_volume_into(VolumeNode& target) const {
        clone_3d_into(target);
        target.shape = shape;
        target.edge_falloff = edge_falloff;
    }
};

// Extra fog inside the volume, on top of the global height fog from the
// VolumetricFogNode. Volumes add: overlapping ones are thicker, and colour and
// scattering average across them by how much density each contributes.
struct FogVolumeNode : VolumeNode {
    float density{0.05f};
    glm::vec3 fog_color{0.8f, 0.85f, 0.9f};
    /// Which volumes a day profile or a weather event moves together. A
    /// `fog_density.<group>` track scales every volume tagged with it and
    /// leaves the rest alone, so one profile can raise the river mist without
    /// touching the fog in a cellar. Empty means nothing but the scene drives
    /// this volume.
    std::string fog_group;
    /// Scales this volume's share of the scattering, leaving the global
    /// scattering as the baseline: brighter or dimmer without being thicker.
    float scattering_mult{1.0f};

    // The fog inside the volume is broken up by a noise field that drifts, so
    // a bank churns rather than sitting still.
    /// How broken up the fog looks, 0 for none. Does not change how much fog
    /// there is: the field averages to one whatever this is set to.
    float noise_contrast{0.0f};
    /// Metres per noise period — the size of a wisp.
    float noise_scale{8.0f};
    /// The way the field drifts. Normalised on the way to the GPU, so its
    /// length does not matter and speed is the only thing that sets the pace.
    glm::vec3 noise_direction{1.0f, 0.05f, 0.3f};
    /// How fast it drifts, in metres per second.
    float noise_speed{0.3f};
    /// How far the weather's wind takes over: it turns the drift toward the
    /// wind's heading and trades the authored speed for the wind's own, so at
    /// 1 the fog runs downwind at wind speed and a calm day nearly stills it.
    /// 0 leaves the volume entirely authored, for a cave or an interior.
    float wind_influence{0.0f};
    /// Metres per second of drift per metre per second of wind, at full
    /// influence. Fog does not travel at the speed of the air that carries it.
    float wind_speed_scale{0.15f};
    /// Octaves of detail. Each one doubles the cost of sampling the field.
    int noise_octaves{2};

    /// How far the volume answers things moving through it, 0 for not at all.
    /// A runner drags the fog along behind them and leaves it briefly thinner
    /// where they passed; both fade as the motion field forgets. Ground mist
    /// wants this, a distant bank does not.
    float motion_response{0.0f};

    /// How hard the rolls a body sheds turn the air behind it, 0 for none.
    /// Something moving through air separates the flow off one side and then
    /// the other, and each separation rolls up turning against the last, so a
    /// wake trails two rows of counter-spinning eddies rather than a line.
    ///
    /// Deliberately not physical. A real roll off a walker is about as wide as
    /// the field's own texels and would read as nothing, so these are scaled
    /// until they can be seen — a dial for how much of the effect is wanted
    /// rather than a measurement of air.
    float vortex_strength{0.0f};

    /// Paints the air the density field has stirred — red where it was shoved
    /// aside, blue where it piled up — so what the field holds can be looked
    /// at directly. An aid while tuning, not something a scene ships with.
    /// How much the air piling up thickens the fog, 0 for not at all. A roll
    /// gathers air on one side and thins it on the other, and showing that is
    /// most of what makes a wake read as something happening rather than as a
    /// patch of fog sliding about.
    float compression{0.0f};

    float density_field_debug{0.0f};

    /// How far the field has drifted so far, in world metres. Integrated each
    /// frame rather than derived from a clock, because the direction and the
    /// speed both move with the wind and a field that jumped when the weather
    /// turned would tear. Runtime state, not saved.
    glm::vec3 noise_drift_{0.0f};

    [[nodiscard]] std::unique_ptr<Node> clone() const override {
        auto copy = std::make_unique<FogVolumeNode>();
        clone_volume_into(*copy);
        copy->density = density;
        copy->fog_color = fog_color;
        copy->fog_group = fog_group;
        copy->scattering_mult = scattering_mult;
        copy->noise_contrast = noise_contrast;
        copy->noise_scale = noise_scale;
        copy->noise_direction = noise_direction;
        copy->noise_speed = noise_speed;
        copy->wind_influence = wind_influence;
        copy->wind_speed_scale = wind_speed_scale;
        copy->noise_octaves = noise_octaves;
        copy->motion_response = motion_response;
        copy->vortex_strength = vortex_strength;
        copy->compression = compression;
        copy->density_field_debug = density_field_debug;
        return copy;
    }
};

// ── Environment nodes ────────────────────────────────────────────────────

struct VolumetricFogNode : Node3D {
    float density{0.003f};
    float height_falloff{0.5f};
    float scattering{0.5f};
    float anisotropy{0.7f};        // HG phase g: positive = godrays
    float max_distance{130.0f};
    float temporal_blend{0.9f};
    glm::vec3 fog_color{0.8f, 0.85f, 0.9f};

    /// How far the height fog answers things moving through it, 0 for not at
    /// all. Having no wisps of its own to displace, it answers by giving way:
    /// thinner along the path something took and a little heavier along the
    /// flanks, so the air reads as having been shoved rather than removed.
    /// Reaches no higher than what came through did, so it is a clearing at a
    /// walker's knees and not a tube bored through the haze.
    ///
    /// Reads faintly in an open scene however high it is set, because the
    /// brightness of a haze reaching hundreds of metres is set by the whole
    /// column and a walker disturbs a few metres of it — what is lost to
    /// thinner air is very nearly repaid by light from the fog behind it.
    /// Somewhere close and enclosed it has a share of the column worth
    /// noticing. A fog volume is the place to look for a wake that carries.
    float motion_response{0.0f};

    /// Paints the air the density field has stirred — red where it was shoved
    /// aside, blue where it piled up — so what the field holds can be looked
    /// at directly rather than inferred from what it does to the fog. An aid
    /// while tuning, not something a scene ships with.
    float density_field_debug{0.0f};

    // Analytic sun shafts (directional light crepuscular rays, screen-space ray-march)
    bool shafts_enabled{true};
    float shaft_intensity{1.0f};
    float shaft_density_mult{1.0f};
    float shaft_max_reach{200.0f};
    int shaft_step_count{32};
    float shaft_anisotropy{0.75f};
    // Blend between an isotropic phase and the forward HG lobe. 1 = pure
    // forward lobe, shafts only read when looking toward the sun. Lower
    // values add a direction-independent floor so shadow-bounded columns
    // stay visible from the side and from above (top-down / isometric).
    float shaft_directionality{1.0f};
    // Fraction of the unshadowed in-scatter subtracted before output. A ray
    // that never crosses a shadow keeps its full brightness; a ray that is
    // partly shadowed drops off faster than its shadowed share, so beams and
    // shadow wedges stay readable once the haze is added onto a bright scene.
    // Not physical: an art control, 0 leaves the march untouched.
    float shaft_threshold{0.0f};
    // The cone around the sun that shafts are allowed to fill, as cosines of
    // the angle from the sun direction. This limits where shafts are drawn —
    // it paints no glow of its own. Without it a pixel takes shafts from
    // whatever direction the sun is, however wide the view angle, and the
    // result is a visible "cone of light" gradient across the ground.
    float shaft_cone_inner{0.95f};  // cos(angle): shafts at full strength within this cone
    float shaft_cone_outer{0.6f};   // cos(angle): shafts fade to zero here (wider = more sky coverage)

    [[nodiscard]] std::unique_ptr<Node> clone() const override {
        auto copy = std::make_unique<VolumetricFogNode>();
        clone_3d_into(*copy);
        copy->density = density;
        copy->height_falloff = height_falloff;
        copy->scattering = scattering;
        copy->anisotropy = anisotropy;
        copy->max_distance = max_distance;
        copy->temporal_blend = temporal_blend;
        copy->fog_color = fog_color;
        copy->motion_response = motion_response;
        copy->density_field_debug = density_field_debug;
        copy->shafts_enabled = shafts_enabled;
        copy->shaft_intensity = shaft_intensity;
        copy->shaft_density_mult = shaft_density_mult;
        copy->shaft_max_reach = shaft_max_reach;
        copy->shaft_step_count = shaft_step_count;
        copy->shaft_anisotropy = shaft_anisotropy;
        copy->shaft_directionality = shaft_directionality;
        copy->shaft_threshold = shaft_threshold;
        copy->shaft_cone_inner = shaft_cone_inner;
        copy->shaft_cone_outer = shaft_cone_outer;
        return copy;
    }
};

// ── World environment ────────────────────────────────────────────────────

struct WorldEnvironmentNode : Node3D {
    glm::vec3 sky_top_color{0.15f, 0.25f, 0.45f};
    glm::vec3 sky_bottom_color{0.8f, 0.4f, 0.1f};
    float ibl_intensity{1.0f};
    std::string hdri_path;
    // .fjsl path (shader_type sky;) — when non-empty, overrides all built-in
    // sky modes. Compiled lazily by SkyPass; supports hot reload.
    std::string sky_shader;

    // Contact shadows: screen-space ray march along the sun direction.
    bool contact_shadows_enabled{true};
    float contact_shadow_distance{0.3f};    // world-space march length in meters
    float contact_shadow_thickness{0.05f};  // occluder thickness tolerance
    int contact_shadow_steps{12};

    // Procedural atmospheric scattering. When enabled, overrides gradient/HDRI.
    bool atmosphere_enabled{false};
    float atmosphere_rayleigh_scale{8500.0f};   // scale height in meters
    float atmosphere_mie_scale{1200.0f};        // scale height in meters
    float atmosphere_mie_g{0.76f};              // Henyey-Greenstein asymmetry
    float atmosphere_mie_coeff{21e-6f};         // haze scatter coefficient
    float atmosphere_sun_intensity{22.0f};      // multiplies directional light color
    float atmosphere_hdri_floor{0.1f};          // minimum HDRI visibility; HDRI × max(tint, floor)

    // Time of day. When enabled, the scene's sun (its first directional light)
    // follows the sun path for the current hour instead of its authored
    // direction; its colour, intensity and ambient stay authored. The hour
    // advances while playing, and in the editor only on request.
    bool time_of_day_enabled{false};
    float time_of_day{12.0f};          // hours, 0..24
    float day_length{1200.0f};         // real seconds per 24 hours; 0 freezes the clock
    bool advance_in_editor{false};
    float sun_path_tilt{40.0f};        // degrees: 0 = noon sun overhead, 90 = circling the horizon
    float north_yaw{0.0f};             // degrees the whole path turns about the up axis
    float noon_hour{12.0f};            // the hour the sun peaks

    // The moon rides the same path as the sun, and its phase is what places
    // it there: new at 0 and 1, first quarter at 0.25, full at 0.5, last
    // quarter at 0.75. One value, because the phase *is* the angle between
    // the two bodies — the disc is shaded by where the sun actually is, so a
    // separate offset would light a shape the phase disagrees with. Colour
    // and intensity are its light at full, before the day profile's moon
    // tracks.
    float moon_phase{0.5f};
    glm::vec3 moon_color{0.55f, 0.65f, 0.9f};
    float moon_intensity{0.08f};

    // The look over the day: a .fjday whose tracks shape the sun, ambient,
    // sky, fog and exposure by the hour. The environment subsystem binds the
    // path to the loaded asset; the binding is runtime state, not saved.
    std::string day_profile;
    DayProfileAssetHandle day_profile_handle{};
    const DayProfileAsset* day_profile_asset{nullptr};
    std::string day_profile_bound;     // the path day_profile_asset was loaded from

    // The weather that plays when nothing else decides: a .fjweather event,
    // started when the scene loads or the path changes and held in its
    // sustain. Bound at runtime like the day profile.
    std::string weather;
    WeatherAssetHandle weather_handle{};
    const WeatherAsset* weather_asset{nullptr};
    std::string weather_bound;

    // Which phase that event opens in. Approach is weather arriving, and the
    // scene spends the event's approach and onset getting there; any later
    // phase is weather that set in before the scene did, so the storm is
    // overhead on the first frame. The phases before it are skipped, not
    // fast-forwarded — see WeatherPlayer::play.
    WeatherPhase weather_start_phase{WeatherPhase::Approach};

    // Who decides the weather: a .fjclimate whose states the director rolls
    // between. With one set, the weather field above is only the event the
    // scene opens on. Bound at runtime like the others.
    std::string climate;
    ClimateAssetHandle climate_handle{};
    const ClimateAsset* climate_asset{nullptr};
    std::string climate_bound;

    // Values for the sky shader's uniforms, by name, as the picker shows
    // them (colours in sRGB). A uniform without an entry keeps the .fjsl
    // default; a day profile track of the same name takes precedence.
    struct SkyUniform {
        std::string name;
        uint32_t count{1};
        std::array<float, 4> values{};
    };
    std::vector<SkyUniform> sky_uniforms;

    [[nodiscard]] std::unique_ptr<Node> clone() const override {
        auto copy = std::make_unique<WorldEnvironmentNode>();
        clone_3d_into(*copy);
        copy->sky_top_color = sky_top_color;
        copy->sky_bottom_color = sky_bottom_color;
        copy->ibl_intensity = ibl_intensity;
        copy->hdri_path = hdri_path;
        copy->sky_shader = sky_shader;
        copy->contact_shadows_enabled = contact_shadows_enabled;
        copy->contact_shadow_distance = contact_shadow_distance;
        copy->contact_shadow_thickness = contact_shadow_thickness;
        copy->contact_shadow_steps = contact_shadow_steps;
        copy->atmosphere_enabled = atmosphere_enabled;
        copy->atmosphere_rayleigh_scale = atmosphere_rayleigh_scale;
        copy->atmosphere_mie_scale = atmosphere_mie_scale;
        copy->atmosphere_mie_g = atmosphere_mie_g;
        copy->atmosphere_mie_coeff = atmosphere_mie_coeff;
        copy->atmosphere_sun_intensity = atmosphere_sun_intensity;
        copy->atmosphere_hdri_floor = atmosphere_hdri_floor;
        copy->time_of_day_enabled = time_of_day_enabled;
        copy->time_of_day = time_of_day;
        copy->day_length = day_length;
        copy->advance_in_editor = advance_in_editor;
        copy->sun_path_tilt = sun_path_tilt;
        copy->north_yaw = north_yaw;
        copy->noon_hour = noon_hour;
        copy->moon_phase = moon_phase;
        copy->moon_color = moon_color;
        copy->moon_intensity = moon_intensity;
        copy->day_profile = day_profile;
        copy->weather = weather;
        copy->climate = climate;
        copy->sky_uniforms = sky_uniforms;
        return copy;
    }
};

} // namespace fjell
