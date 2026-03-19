#pragma once

#include "scene/scene.hpp"

#include <glm/gtc/matrix_transform.hpp>

namespace fjell {

struct CameraNode : SceneNode {
    float fov{45.0f};
    float near_plane{0.1f};
    float far_plane{1000.0f};

    [[nodiscard]] glm::mat4 view_matrix() const {
        glm::vec3 pos = glm::vec3(world_matrix_[3]);
        // Camera looks down -Z in local space
        glm::vec3 forward = -glm::normalize(glm::vec3(world_matrix_[2]));
        glm::vec3 up = glm::normalize(glm::vec3(world_matrix_[1]));
        return glm::lookAt(pos, pos + forward, up);
    }

    [[nodiscard]] glm::mat4 projection_matrix(float aspect) const {
        auto proj = glm::perspective(glm::radians(fov), aspect, near_plane, far_plane);
        proj[1][1] *= -1.0f; // Vulkan Y-flip
        return proj;
    }

    [[nodiscard]] glm::vec3 position() const {
        return glm::vec3(world_matrix_[3]);
    }

    [[nodiscard]] std::unique_ptr<SceneNode> clone() const override {
        auto copy = std::make_unique<CameraNode>();
        clone_base_into(*copy);
        copy->fov = fov;
        copy->near_plane = near_plane;
        copy->far_plane = far_plane;
        return copy;
    }
};

} // namespace fjell
