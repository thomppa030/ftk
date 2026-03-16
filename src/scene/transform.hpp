#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

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

/// Decompose a TRS matrix back into position/rotation/scale.
/// Matches the Euler XYZ convention used by Transform::matrix().
[[nodiscard]] inline Transform decompose_matrix(const glm::mat4& m) {
    Transform t;
    t.position = glm::vec3(m[3]);

    glm::vec3 c0{m[0][0], m[0][1], m[0][2]};
    glm::vec3 c1{m[1][0], m[1][1], m[1][2]};
    glm::vec3 c2{m[2][0], m[2][1], m[2][2]};

    t.scale.x = glm::length(c0);
    t.scale.y = glm::length(c1);
    t.scale.z = glm::length(c2);

    glm::mat3 rot(c0 / t.scale.x, c1 / t.scale.y, c2 / t.scale.z);

    t.rotation.x = glm::degrees(std::atan2(rot[2][1], rot[2][2]));
    t.rotation.y = glm::degrees(std::atan2(-rot[2][0],
        std::sqrt(rot[2][1] * rot[2][1] + rot[2][2] * rot[2][2])));
    t.rotation.z = glm::degrees(std::atan2(rot[1][0], rot[0][0]));

    return t;
}

} // namespace fjell
