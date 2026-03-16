#include "renderer/fly_camera.hpp"
#include "core/input.hpp"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace fjell {

static constexpr glm::vec3 WORLD_UP{0.0f, 1.0f, 0.0f};

FlyCamera::FlyCamera(glm::vec3 position, float yaw, float pitch)
    : position_{position}, yaw_{yaw}, pitch_{pitch} {
    update_vectors();
}

void FlyCamera::update(const Input& input, float dt) {
    // Mouse look
    auto mouse_d = input.mouse_delta();
    yaw_ += static_cast<float>(mouse_d.x) * mouse_sensitivity_;
    pitch_ -= static_cast<float>(mouse_d.y) * mouse_sensitivity_;
    pitch_ = std::clamp(pitch_, -89.0f, 89.0f);
    update_vectors();

    // Movement
    float speed = move_speed_ * dt;
    if (input.is_key_held(GLFW_KEY_LEFT_SHIFT) ||
        input.is_key_held(GLFW_KEY_RIGHT_SHIFT)) {
        speed *= 3.0f;
    }

    if (input.is_key_held(GLFW_KEY_W)) position_ += front_ * speed;
    if (input.is_key_held(GLFW_KEY_S)) position_ -= front_ * speed;
    if (input.is_key_held(GLFW_KEY_A)) position_ -= right_ * speed;
    if (input.is_key_held(GLFW_KEY_D)) position_ += right_ * speed;
    if (input.is_key_held(GLFW_KEY_E)) position_ += WORLD_UP * speed;
    if (input.is_key_held(GLFW_KEY_Q)) position_ -= WORLD_UP * speed;
}

glm::mat4 FlyCamera::view_matrix() const {
    return glm::lookAt(position_, position_ + front_, WORLD_UP);
}

glm::mat4 FlyCamera::projection_matrix(float aspect) const {
    auto proj = glm::perspective(glm::radians(fov_), aspect, near_, far_);
    proj[1][1] *= -1; // Vulkan Y-flip
    return proj;
}

void FlyCamera::update_vectors() {
    float yaw_rad = glm::radians(yaw_);
    float pitch_rad = glm::radians(pitch_);

    front_ = glm::normalize(glm::vec3{
        std::cos(pitch_rad) * std::cos(yaw_rad),
        std::sin(pitch_rad),
        std::cos(pitch_rad) * std::sin(yaw_rad)
    });
    right_ = glm::normalize(glm::cross(front_, WORLD_UP));
    up_ = glm::normalize(glm::cross(right_, front_));
}

} // namespace fjell
