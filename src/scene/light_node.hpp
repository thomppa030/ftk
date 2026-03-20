#pragma once

#include "scene/scene.hpp"

namespace fjell {

// ── Light nodes ─────────────────────────────────────────────────────────

enum class LightType : uint8_t { Directional, Point, Spot };

struct DirectionalLightNode : SceneNode {
    glm::vec3 direction{-0.3f, -1.0f, -0.5f};
    glm::vec3 color{1.0f, 1.0f, 0.95f};
    float intensity{1.5f};
    glm::vec3 ambient_color{0.1f, 0.1f, 0.12f};
    float ambient_intensity{1.0f};

    [[nodiscard]] std::unique_ptr<SceneNode> clone() const override {
        auto copy = std::make_unique<DirectionalLightNode>();
        clone_base_into(*copy);
        copy->direction = direction;
        copy->color = color;
        copy->intensity = intensity;
        copy->ambient_color = ambient_color;
        copy->ambient_intensity = ambient_intensity;
        return copy;
    }
};

struct PointLightNode : SceneNode {
    glm::vec3 color{1.0f};
    float intensity{1.0f};
    float radius{10.0f};

    [[nodiscard]] std::unique_ptr<SceneNode> clone() const override {
        auto copy = std::make_unique<PointLightNode>();
        clone_base_into(*copy);
        copy->color = color;
        copy->intensity = intensity;
        copy->radius = radius;
        return copy;
    }
};

struct SpotLightNode : SceneNode {
    glm::vec3 direction{0.0f, -1.0f, 0.0f};
    glm::vec3 color{1.0f};
    float intensity{1.0f};
    float radius{10.0f};
    float inner_angle{20.0f};
    float outer_angle{30.0f};

    [[nodiscard]] std::unique_ptr<SceneNode> clone() const override {
        auto copy = std::make_unique<SpotLightNode>();
        clone_base_into(*copy);
        copy->direction = direction;
        copy->color = color;
        copy->intensity = intensity;
        copy->radius = radius;
        copy->inner_angle = inner_angle;
        copy->outer_angle = outer_angle;
        return copy;
    }
};

// ── Environment nodes ────────────────────────────────────────────────────

struct VolumetricFogNode : SceneNode {
    float density{0.003f};
    float height_falloff{0.5f};
    float scattering{0.5f};
    float anisotropy{0.7f};        // HG phase g: positive = godrays
    float max_distance{130.0f};
    float temporal_blend{0.9f};
    glm::vec3 fog_color{0.8f, 0.85f, 0.9f};

    [[nodiscard]] std::unique_ptr<SceneNode> clone() const override {
        auto copy = std::make_unique<VolumetricFogNode>();
        clone_base_into(*copy);
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

} // namespace fjell
