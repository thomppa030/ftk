#pragma once

#include <glm/glm.hpp>

namespace fjell {

class Input;

class FlyCamera {
public:
    FlyCamera(glm::vec3 position, float yaw, float pitch);

    void update(const Input& input, float dt);

    [[nodiscard]] glm::mat4 view_matrix() const;
    [[nodiscard]] glm::mat4 projection_matrix(float aspect) const;
    [[nodiscard]] glm::vec3 position() const { return position_; }
    [[nodiscard]] float fov() const { return fov_; }

private:
    void update_vectors();

    glm::vec3 position_;
    float yaw_;   // degrees
    float pitch_; // degrees

    glm::vec3 front_;
    glm::vec3 right_;
    glm::vec3 up_;

    float move_speed_{5.0f};
    float mouse_sensitivity_{0.15f};
    float fov_{45.0f};
    float near_{0.1f};
    float far_{1000.0f};
};

} // namespace fjell
