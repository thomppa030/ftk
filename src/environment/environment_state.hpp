#pragma once

#include <glm/glm.hpp>

#include <string>

namespace fjell {

// ── Resolved environment ─────────────────────────────────────────────────
//
// Everything the renderer needs to know about the world's lighting and
// atmosphere for one frame, resolved once from the scene's authoring nodes
// (directional light, world environment, volumetric fog). Passes read this
// struct and never the nodes: the value a pass sees is the finished one, after
// every rule that shapes it has run, and a new rule is added in one place.
//
// Colours are linear light, ready to multiply. Directions toward a body are
// normalized and point from the surface to the sun or moon.

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

struct EnvironmentState {
    DirectionalBody sun;
    DirectionalBody moon;

    /// The body that casts the cascaded shadows and fills the global UBO's
    /// directional light this frame.
    DirectionalBody primary;
    /// The other body, lit without cascades.
    DirectionalBody secondary;

    glm::vec3 ambient_color{0.1f, 0.1f, 0.12f};
    float ambient_intensity{0.4f};

    SkySettings sky;
    ContactShadowSettings contact_shadows;
    FogSettings fog;
    SunShaftSettings sun_shafts;
};

} // namespace fjell
