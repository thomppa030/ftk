#pragma once

#include "scene/scene.hpp"

namespace fjell {

// ── Light nodes ─────────────────────────────────────────────────────────

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
        return copy;
    }
};

} // namespace fjell
