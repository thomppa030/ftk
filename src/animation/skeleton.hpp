#pragma once

#include "animation/fjskel.hpp"

#include <ozz/animation/runtime/skeleton.h>

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace fjell {

class Skeleton {
public:
    [[nodiscard]] static Skeleton load(const std::string& path);
    [[nodiscard]] static Skeleton from_joints(const std::vector<FjSkelJoint>& joints);
    void save(const std::string& path) const;

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

private:
    ozz::animation::Skeleton skeleton_;
    std::vector<glm::mat4> inverse_bind_matrices_;
    std::vector<FjSkelJoint> joints_;
    std::vector<BoneSlot> slots_;
};

} // namespace fjell
