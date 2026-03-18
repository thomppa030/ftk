#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>

namespace fjell {

struct Transform {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; // identity quaternion
    glm::vec3 scale{1.0f};

    // Euler angles in degrees — convenience for UI and serialization
    [[nodiscard]] glm::vec3 euler_degrees() const {
        return glm::degrees(glm::eulerAngles(rotation));
    }

    void set_euler_degrees(const glm::vec3& degrees) {
        rotation = glm::quat(glm::radians(degrees));
    }

    [[nodiscard]] glm::mat4 matrix() const {
        auto m = glm::translate(glm::mat4(1.0f), position);
        m *= glm::mat4_cast(rotation);
        m = glm::scale(m, scale);
        return m;
    }
};

/// Decompose a TRS matrix back into position/rotation(quat)/scale.
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
    t.rotation = glm::quat_cast(rot);

    return t;
}

} // namespace fjell
