#pragma once

// .fjskel JSON format — Fjell engine skeleton format.
// Shared between the engine runtime and the fjimport tool.
//
// JSON schema:
// {
//     "joints": [
//         {
//             "name": "Hips",
//             "parent": -1,
//             "translation": [0.0, 0.0, 0.0],
//             "rotation": [0.0, 0.0, 0.0, 1.0],
//             "scale": [1.0, 1.0, 1.0]
//         },
//         ...
//     ],
//     "slots": []
// }
//
// Joints are stored in depth-first order. Parent is an index into the
// joints array (-1 for root joints). Translation/rotation/scale define
// the joint's rest pose in parent-local space. Rotation is a quaternion
// stored as [x, y, z, w].
//
// Inverse bind matrices are NOT stored — they are computed at load time
// from the rest pose hierarchy.
//
// "slots" is reserved for bone attachment slots (animation editor).

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>

namespace fjell {

struct FjSkelJoint {
    std::string name;
    int parent{-1};
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; // w, x, y, z (glm order)
    glm::vec3 scale{1.0f};
};

struct BoneSlot {
    std::string name;
    int bone_index{-1};
    glm::vec3 offset_translation{0.0f};
    glm::quat offset_rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 offset_scale{1.0f};
};

} // namespace fjell
