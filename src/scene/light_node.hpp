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

} // namespace fjell
