#pragma once

#include "renderer/resources/resource_handle.hpp"
#include "scene/scene.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace fjell {

struct DayProfileAsset;

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

// ── Environment nodes ────────────────────────────────────────────────────

struct VolumetricFogNode : Node3D {
    float density{0.003f};
    float height_falloff{0.5f};
    float scattering{0.5f};
    float anisotropy{0.7f};        // HG phase g: positive = godrays
    float max_distance{130.0f};
    float temporal_blend{0.9f};
    glm::vec3 fog_color{0.8f, 0.85f, 0.9f};

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
    float shaft_halo_inner{0.95f};  // cos(angle): full-strength shafts within this cone around the sun
    float shaft_halo_outer{0.6f};   // cos(angle): shafts fade to zero here (wider = more sky coverage)

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
        copy->shafts_enabled = shafts_enabled;
        copy->shaft_intensity = shaft_intensity;
        copy->shaft_density_mult = shaft_density_mult;
        copy->shaft_max_reach = shaft_max_reach;
        copy->shaft_step_count = shaft_step_count;
        copy->shaft_anisotropy = shaft_anisotropy;
        copy->shaft_directionality = shaft_directionality;
        copy->shaft_threshold = shaft_threshold;
        copy->shaft_halo_inner = shaft_halo_inner;
        copy->shaft_halo_outer = shaft_halo_outer;
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

    // The moon rides the same path, so many hours behind the sun, and its
    // phase sets how bright it is: new at 0 and 1, full at 0.5. Colour and
    // intensity are its light at full, before the day profile's moon tracks.
    float moon_offset_hours{12.0f};
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
        copy->moon_offset_hours = moon_offset_hours;
        copy->moon_phase = moon_phase;
        copy->moon_color = moon_color;
        copy->moon_intensity = moon_intensity;
        copy->day_profile = day_profile;
        copy->sky_uniforms = sky_uniforms;
        return copy;
    }
};

} // namespace fjell
