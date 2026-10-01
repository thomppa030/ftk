#pragma once

#include "animation/fjskel.hpp"
#include "physics/collision_shape.hpp"
#include "ftk/base/result.hpp"

#include <ozz/animation/runtime/skeleton.h>

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace fjell {

class Skeleton {
public:
    [[nodiscard]] static ftk::Result<Skeleton> load(const std::string& path);
    [[nodiscard]] static ftk::Result<Skeleton> from_joints(const std::vector<FjSkelJoint>& joints);
    [[nodiscard]] ftk::Result<> save(const std::string& path) const;

    [[nodiscard]] const ozz::animation::Skeleton& raw() const { return skeleton_; }
    [[nodiscard]] int num_joints() const { return skeleton_.num_joints(); }
    [[nodiscard]] int num_soa_joints() const { return skeleton_.num_soa_joints(); }

    [[nodiscard]] int find_joint(const std::string& name) const;

    [[nodiscard]] const std::vector<glm::mat4>& inverse_bind_matrices() const {
        return inverse_bind_matrices_;
    }

    [[nodiscard]] const std::vector<FjSkelJoint>& joints() const { return joints_; }
    [[nodiscard]] const std::vector<BoneSlot>& slots() const { return slots_; }
    [[nodiscard]] std::vector<BoneSlot>& slots() { return slots_; }

    /// Collision shapes carried by this skeleton's bones. Every character using
    /// the skeleton collides as these.
    [[nodiscard]] const std::vector<BoneCollider>& colliders() const { return colliders_; }
    [[nodiscard]] std::vector<BoneCollider>& colliders() { return colliders_; }

private:
    ozz::animation::Skeleton skeleton_;
    std::vector<glm::mat4> inverse_bind_matrices_;
    std::vector<FjSkelJoint> joints_;
    std::vector<BoneSlot> slots_;
    std::vector<BoneCollider> colliders_;
};

} // namespace fjell
