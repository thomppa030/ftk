#pragma once

// .fjanim JSON format — Fjell engine animation clip format.
// Shared between the engine runtime and the fjimport tool.
//
// JSON schema:
// {
//     "name": "Idle_Loop",
//     "tracks": [
//         {
//             "translations": [[0.0, [0.0, 0.5, 0.0]], ...],
//             "rotations": [[0.0, [0.0, 0.0, 0.0, 1.0]], ...],
//             "scales": [[0.0, [1.0, 1.0, 1.0]], ...]
//         },
//         ...
//     ],
//     "events": []
// }
//
// Each track corresponds to one joint (same order as .fjskel joints).
// Keyframes are [time, value] pairs. Rotation values are [x, y, z, w].
// Duration is computed from the latest keyframe time across all tracks.
//
// "events" is reserved for animation event markers (animation editor).

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>
#include <vector>

namespace fjell {

struct AnimKeyVec3 {
    float time{0.0f};
    glm::vec3 value{0.0f};
};

struct AnimKeyQuat {
    float time{0.0f};
    glm::quat value{1.0f, 0.0f, 0.0f, 0.0f}; // w, x, y, z (glm order)
};

struct AnimJointTrack {
    std::vector<AnimKeyVec3> translations;
    std::vector<AnimKeyQuat> rotations;
    std::vector<AnimKeyVec3> scales;
};

} // namespace fjell
