#pragma once

#include "scene/scene.hpp"

#include <glm/gtc/matrix_transform.hpp>

namespace fjell {

enum class CameraProjection : uint8_t {
    perspective,
    orthographic,
};

struct CameraNode : Node3D {
    CameraProjection projection{CameraProjection::perspective};

    // Perspective
    float fov{45.0f};           // vertical FOV in degrees

    // Orthographic
    float ortho_size{10.0f};    // half-height in world units

    // Shared
    float near_plane{0.1f};
    float far_plane{1000.0f};

    // Raw override (replaces computed projection entirely when set)
    bool use_projection_override{false};
    glm::mat4 projection_override{1.0f};

    [[nodiscard]] glm::mat4 view_matrix() const {
        glm::vec3 pos = glm::vec3(world_matrix_[3]);
        // Camera looks down -Z in local space
        glm::vec3 forward = -glm::normalize(glm::vec3(world_matrix_[2]));
        glm::vec3 up = glm::normalize(glm::vec3(world_matrix_[1]));
        return glm::lookAt(pos, pos + forward, up);
    }

    [[nodiscard]] glm::mat4 projection_matrix(float aspect) const {
        if (use_projection_override) return projection_override;

        glm::mat4 proj;
        if (projection == CameraProjection::perspective) {
            proj = glm::perspectiveZO(glm::radians(fov), aspect, near_plane, far_plane);
        } else {
            float half_h = ortho_size;
            float half_w = ortho_size * aspect;
            proj = glm::orthoZO(-half_w, half_w, -half_h, half_h, near_plane, far_plane);
        }
        proj[1][1] *= -1.0f; // Vulkan Y-flip
        return proj;
    }

    [[nodiscard]] glm::vec3 position() const {
        return glm::vec3(world_matrix_[3]);
    }

    [[nodiscard]] std::unique_ptr<Node> clone() const override {
        auto copy = std::make_unique<CameraNode>();
        clone_3d_into(*copy);
        copy->projection = projection;
        copy->fov = fov;
        copy->ortho_size = ortho_size;
        copy->near_plane = near_plane;
        copy->far_plane = far_plane;
        copy->use_projection_override = use_projection_override;
        copy->projection_override = projection_override;
        return copy;
    }
};

} // namespace fjell
