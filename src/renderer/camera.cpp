#include "renderer/camera.hpp"
#include "core/input.hpp"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace fjell {

Camera::Camera(glm::vec3 target, float distance, float yaw, float pitch)
    : target_{target}, yaw_{yaw}, pitch_{pitch}, distance_{distance} {
    update_position();
}

void Camera::focus(const glm::vec3& target, float object_radius) {
    focusing_ = true;
    focus_from_ = target_;
    focus_to_ = target;
    focus_dist_from_ = distance_;
    // Frame the object: distance so it fills ~60% of the view
    float ideal_dist = object_radius / std::tan(glm::radians(fov_ * 0.5f)) * 1.5f;
    focus_dist_to_ = std::clamp(ideal_dist, min_distance_, max_distance_);
    focus_t_ = 0.0f;
}

void Camera::update_focus(float dt) {
    if (!focusing_) return;

    focus_t_ += dt * 4.0f; // ~0.25s transition
    if (focus_t_ >= 1.0f) {
        focus_t_ = 1.0f;
        focusing_ = false;
    }

    // Smooth ease-out
    float t = 1.0f - (1.0f - focus_t_) * (1.0f - focus_t_);
    target_ = glm::mix(focus_from_, focus_to_, t);
    distance_ = glm::mix(focus_dist_from_, focus_dist_to_, t);
}

void Camera::update(const Input& input, float dt) {
    auto mouse_d = input.mouse_delta();
    auto scroll_d = input.scroll_delta();

    bool middle_held = input.is_mouse_button_held(GLFW_MOUSE_BUTTON_MIDDLE);
    bool shift_held = input.is_key_held(GLFW_KEY_LEFT_SHIFT) ||
                      input.is_key_held(GLFW_KEY_RIGHT_SHIFT);

    // Cancel focus animation on any camera input
    if (middle_held || scroll_d.y != 0.0 ||
        input.is_key_held(GLFW_KEY_W) || input.is_key_held(GLFW_KEY_S) ||
        input.is_key_held(GLFW_KEY_A) || input.is_key_held(GLFW_KEY_D) ||
        input.is_key_held(GLFW_KEY_Q) || input.is_key_held(GLFW_KEY_E)) {
        focusing_ = false;
    }

    update_focus(dt);

    // WASD movement — moves target (and camera) through space
    {
        glm::vec3 forward = glm::normalize(target_ - position_);
        glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3{0.0f, 1.0f, 0.0f}));
        float speed = move_speed_ * dt * (distance_ * 0.5f + 1.0f);

        if (input.is_key_held(GLFW_KEY_W)) target_ += forward * speed;
        if (input.is_key_held(GLFW_KEY_S)) target_ -= forward * speed;
        if (input.is_key_held(GLFW_KEY_A)) target_ -= right * speed;
        if (input.is_key_held(GLFW_KEY_D)) target_ += right * speed;
        if (input.is_key_held(GLFW_KEY_Q)) target_.y -= speed;
        if (input.is_key_held(GLFW_KEY_E)) target_.y += speed;
    }

    if (middle_held && shift_held) {
        // Pan: shift + middle mouse drag
        glm::vec3 right = glm::normalize(glm::cross(
            glm::normalize(target_ - position_), glm::vec3{0.0f, 1.0f, 0.0f}));
        glm::vec3 up = glm::normalize(glm::cross(right,
            glm::normalize(target_ - position_)));

        float t = (distance_ - min_distance_) / (max_distance_ - min_distance_);
        float pan_scale = pan_sensitivity_ * glm::mix(0.2f, 1.0f, t) * distance_;
        target_ -= right * static_cast<float>(mouse_d.x) * pan_scale;
        target_ += up * static_cast<float>(mouse_d.y) * pan_scale;
    } else if (middle_held) {
        // Orbit: middle mouse drag
        yaw_ += static_cast<float>(mouse_d.x) * orbit_sensitivity_;
        pitch_ += static_cast<float>(mouse_d.y) * orbit_sensitivity_;
        pitch_ = std::clamp(pitch_, -89.0f, 89.0f);
    }

    // Zoom: scroll wheel — speed proportional to distance
    if (scroll_d.y != 0.0) {
        float zoom_scale = zoom_sensitivity_ * (distance_ / max_distance_);
        distance_ -= static_cast<float>(scroll_d.y) * zoom_scale * distance_;
        distance_ = std::clamp(distance_, min_distance_, max_distance_);
    }

    update_position();
}

glm::mat4 Camera::view_matrix() const {
    return glm::lookAt(position_, target_, glm::vec3{0.0f, 1.0f, 0.0f});
}

glm::mat4 Camera::projection_matrix(float aspect) const {
    auto proj = glm::perspective(glm::radians(fov_), aspect, near_, far_);
    proj[1][1] *= -1; // Vulkan Y-flip
    return proj;
}

void Camera::update_position() {
    float yaw_rad = glm::radians(yaw_);
    float pitch_rad = glm::radians(pitch_);

    glm::vec3 offset{
        std::cos(pitch_rad) * std::cos(yaw_rad),
        std::sin(pitch_rad),
        std::cos(pitch_rad) * std::sin(yaw_rad)
    };

    position_ = target_ + offset * distance_;
}

} // namespace fjell
