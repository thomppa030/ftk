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
        return copy;
    }
};

// ── World environment ────────────────────────────────────────────────────

struct WorldEnvironmentNode : Node3D {
    glm::vec3 sky_top_color{0.15f, 0.25f, 0.45f};
    glm::vec3 sky_bottom_color{0.8f, 0.4f, 0.1f};
    float ibl_intensity{1.0f};
    std::string hdri_path;

    [[nodiscard]] std::unique_ptr<Node> clone() const override {
        auto copy = std::make_unique<WorldEnvironmentNode>();
        clone_3d_into(*copy);
        copy->sky_top_color = sky_top_color;
        copy->sky_bottom_color = sky_bottom_color;
        copy->ibl_intensity = ibl_intensity;
        copy->hdri_path = hdri_path;
        return copy;
    }
};

} // namespace fjell
