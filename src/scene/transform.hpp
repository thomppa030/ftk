#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace fjell {

struct Transform {
    glm::vec3 position{0.0f};
    glm::vec3 rotation{0.0f}; // Euler angles in degrees
    glm::vec3 scale{1.0f};

    [[nodiscard]] glm::mat4 matrix() const {
        auto m = glm::translate(glm::mat4(1.0f), position);
        m = glm::rotate(m, glm::radians(rotation.x), {1, 0, 0});
        m = glm::rotate(m, glm::radians(rotation.y), {0, 1, 0});
        m = glm::rotate(m, glm::radians(rotation.z), {0, 0, 1});
        m = glm::scale(m, scale);
        return m;
    }
};

} // namespace fjell
