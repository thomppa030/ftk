#pragma once

#include <glm/glm.hpp>

namespace fjell {

class Input;

class Camera {
public:
    Camera(glm::vec3 target = {0.0f, 0.0f, 0.0f},
           float distance = 10.0f,
           float yaw = -30.0f, float pitch = 25.0f);

    void update(const Input& input, float dt);

    [[nodiscard]] glm::mat4 view_matrix() const;
    [[nodiscard]] glm::mat4 projection_matrix(float aspect) const;

    [[nodiscard]] glm::vec3 position() const { return position_; }
    [[nodiscard]] float fov() const { return fov_; }

private:
    void update_position();

    glm::vec3 target_;
    glm::vec3 position_;

    float yaw_;
    float pitch_;
    float distance_;

    float orbit_sensitivity_{0.3f};
    float pan_sensitivity_{0.02f};
    float zoom_sensitivity_{1.5f};
    float min_distance_{0.5f};
    float max_distance_{500.0f};
    float fov_{45.0f};
    float near_{0.1f};
    float far_{1000.0f};
};

} // namespace fjell
