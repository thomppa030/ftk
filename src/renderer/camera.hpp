#pragma once

#include <glm/glm.hpp>

namespace fjell {

class Input;

class Camera {
public:
    Camera(glm::vec3 position = {0.0f, 0.0f, 3.0f},
           float yaw = -90.0f, float pitch = 0.0f);

    void update(const Input& input, float dt);

    [[nodiscard]] glm::mat4 view_matrix() const;
    [[nodiscard]] glm::mat4 projection_matrix(float aspect) const;

    void set_position(glm::vec3 pos) { position_ = pos; }
    void set_speed(float speed) { speed_ = speed; }
    void set_fov(float fov) { fov_ = fov; }

    [[nodiscard]] glm::vec3 position() const { return position_; }
    [[nodiscard]] float fov() const { return fov_; }

private:
    void update_vectors();

    glm::vec3 position_;
    glm::vec3 front_{0.0f, 0.0f, -1.0f};
    glm::vec3 up_{0.0f, 1.0f, 0.0f};
    glm::vec3 right_{1.0f, 0.0f, 0.0f};

    float yaw_;
    float pitch_;
    float speed_{20.0f};
    float sensitivity_{0.1f};
    float fov_{45.0f};
    float near_{0.1f};
    float far_{1000.0f};
};

} // namespace fjell
