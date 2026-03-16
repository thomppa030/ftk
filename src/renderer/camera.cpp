#include "renderer/camera.hpp"
#include "core/input.hpp"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace fjell {

Camera::Camera(glm::vec3 position, float yaw, float pitch)
    : position_{position}, yaw_{yaw}, pitch_{pitch} {
    update_vectors();
}

void Camera::update(const Input& input, float dt) {
    // Movement
    float velocity = speed_ * dt;

    if (input.is_key_held(GLFW_KEY_W)) position_ += front_ * velocity;
    if (input.is_key_held(GLFW_KEY_S)) position_ -= front_ * velocity;
    if (input.is_key_held(GLFW_KEY_A)) position_ -= right_ * velocity;
    if (input.is_key_held(GLFW_KEY_D)) position_ += right_ * velocity;
    if (input.is_key_held(GLFW_KEY_SPACE)) position_ += up_ * velocity;
    if (input.is_key_held(GLFW_KEY_LEFT_SHIFT)) position_ -= up_ * velocity;

    // Look (only when cursor is captured)
    if (input.is_cursor_captured()) {
        auto delta = input.mouse_delta();
        yaw_ += static_cast<float>(delta.x) * sensitivity_;
        pitch_ -= static_cast<float>(delta.y) * sensitivity_;
        pitch_ = std::clamp(pitch_, -89.0f, 89.0f);
        update_vectors();
    }
}

glm::mat4 Camera::view_matrix() const {
    return glm::lookAt(position_, position_ + front_, up_);
}

glm::mat4 Camera::projection_matrix(float aspect) const {
    auto proj = glm::perspective(glm::radians(fov_), aspect, near_, far_);
    proj[1][1] *= -1; // Vulkan Y-flip
    return proj;
}

void Camera::update_vectors() {
    float yaw_rad = glm::radians(yaw_);
    float pitch_rad = glm::radians(pitch_);

    front_ = glm::normalize(glm::vec3{
        std::cos(yaw_rad) * std::cos(pitch_rad),
        std::sin(pitch_rad),
        std::sin(yaw_rad) * std::cos(pitch_rad)
    });
    right_ = glm::normalize(glm::cross(front_, glm::vec3{0.0f, 1.0f, 0.0f}));
    up_ = glm::normalize(glm::cross(right_, front_));
}

} // namespace fjell
