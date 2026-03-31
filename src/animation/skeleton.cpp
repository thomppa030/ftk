#include "animation/skeleton.hpp"
#include "core/log.hpp"

#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>

#include <nlohmann/json.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <fstream>
#include <stdexcept>

namespace fjell {

namespace {

// Build an ozz RawSkeleton from flat joint array, then compile to runtime
ozz::animation::Skeleton build_ozz_skeleton(const std::vector<FjSkelJoint>& joints) {
    ozz::animation::offline::RawSkeleton raw;

    // Find root joints (parent == -1)
    std::vector<int> roots;
    for (int i = 0; i < static_cast<int>(joints.size()); ++i) {
        if (joints[i].parent == -1) {
            roots.push_back(i);
        }
    }

    // Build children lookup
    std::vector<std::vector<int>> children(joints.size());
    for (int i = 0; i < static_cast<int>(joints.size()); ++i) {
        if (joints[i].parent >= 0) {
            children[joints[i].parent].push_back(i);
        }
    }

    // Recursive helper to fill ozz joint
    std::function<void(int, ozz::animation::offline::RawSkeleton::Joint&)> fill =
        [&](int idx, ozz::animation::offline::RawSkeleton::Joint& out) {
            const auto& j = joints[idx];
            out.name = j.name.c_str();
            out.transform.translation = ozz::math::Float3(j.translation.x, j.translation.y, j.translation.z);
            out.transform.rotation = ozz::math::Quaternion(j.rotation.x, j.rotation.y, j.rotation.z, j.rotation.w);
            out.transform.scale = ozz::math::Float3(j.scale.x, j.scale.y, j.scale.z);

            for (int child : children[idx]) {
                out.children.emplace_back();
                fill(child, out.children.back());
            }
        };

    raw.roots.resize(roots.size());
    for (int i = 0; i < static_cast<int>(roots.size()); ++i) {
        fill(roots[i], raw.roots[i]);
    }

    if (!raw.Validate()) {
        throw std::runtime_error("Invalid skeleton hierarchy");
    }

    ozz::animation::offline::SkeletonBuilder builder;
    auto result = builder(raw);
    if (!result) {
        throw std::runtime_error("Failed to build ozz skeleton");
    }

    // Move out of unique_ptr
    ozz::animation::Skeleton skel = std::move(*result);
    return skel;
}

// Compute inverse bind matrices from rest pose hierarchy
std::vector<glm::mat4> compute_inverse_bind_matrices(
        const std::vector<FjSkelJoint>& joints) {
    int n = static_cast<int>(joints.size());
    std::vector<glm::mat4> world(n, glm::mat4(1.0f));

    for (int i = 0; i < n; ++i) {
        glm::mat4 local = glm::translate(glm::mat4(1.0f), joints[i].translation)
                        * glm::mat4_cast(joints[i].rotation)
                        * glm::scale(glm::mat4(1.0f), joints[i].scale);

        if (joints[i].parent >= 0) {
            world[i] = world[joints[i].parent] * local;
        } else {
            world[i] = local;
        }
    }

    std::vector<glm::mat4> inv_bind(n);
    for (int i = 0; i < n; ++i) {
        inv_bind[i] = glm::inverse(world[i]);
    }
    return inv_bind;
}

} // anonymous namespace

Skeleton Skeleton::load(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("Failed to open skeleton: " + path);
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(file);
    } catch (const nlohmann::json::parse_error& e) {
        throw std::runtime_error("Failed to parse skeleton JSON: " + path + " — " + e.what());
    }

    Skeleton skel;

    // Parse joints
    if (!j.contains("joints") || !j["joints"].is_array()) {
        throw std::runtime_error("Skeleton missing 'joints' array: " + path);
    }

    const auto& jjoints = j["joints"];
    skel.joints_.reserve(jjoints.size());

    for (const auto& jj : jjoints) {
        FjSkelJoint joint;
        joint.name = jj.value("name", "");
        joint.parent = jj.value("parent", -1);

        if (jj.contains("translation")) {
            const auto& t = jj["translation"];
            joint.translation = {t[0].get<float>(), t[1].get<float>(), t[2].get<float>()};
        }
        if (jj.contains("rotation")) {
            const auto& r = jj["rotation"];
            // JSON stores [x, y, z, w], glm::quat constructor is (w, x, y, z)
            joint.rotation = glm::quat(r[3].get<float>(), r[0].get<float>(),
                                       r[1].get<float>(), r[2].get<float>());
        }
        if (jj.contains("scale")) {
            const auto& s = jj["scale"];
            joint.scale = {s[0].get<float>(), s[1].get<float>(), s[2].get<float>()};
        }

        skel.joints_.push_back(std::move(joint));
    }

    // Parse bone slots
    if (j.contains("slots") && j["slots"].is_array()) {
        for (const auto& js : j["slots"]) {
            BoneSlot slot;
            slot.name = js.value("name", "");
            slot.bone_index = js.value("bone_index", -1);

            if (js.contains("offset_translation")) {
                const auto& t = js["offset_translation"];
                slot.offset_translation = {t[0].get<float>(), t[1].get<float>(), t[2].get<float>()};
            }
            if (js.contains("offset_rotation")) {
                const auto& r = js["offset_rotation"];
                slot.offset_rotation = glm::quat(r[3].get<float>(), r[0].get<float>(),
                                                  r[1].get<float>(), r[2].get<float>());
            }
            if (js.contains("offset_scale")) {
                const auto& s = js["offset_scale"];
                slot.offset_scale = {s[0].get<float>(), s[1].get<float>(), s[2].get<float>()};
            }

            skel.slots_.push_back(std::move(slot));
        }
    }

    // Warn on unknown top-level fields
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (it.key() != "joints" && it.key() != "slots") {
            FJELL_CORE_WARN("Skeleton {}: unknown field '{}'", path, it.key());
        }
    }

    // Build ozz runtime skeleton from joint data
    skel.skeleton_ = build_ozz_skeleton(skel.joints_);

    // Compute inverse bind matrices from rest pose hierarchy
    skel.inverse_bind_matrices_ = compute_inverse_bind_matrices(skel.joints_);

    FJELL_CORE_INFO("Loaded skeleton: {} ({} joints)", path, skel.num_joints());
    return skel;
}

Skeleton Skeleton::from_joints(const std::vector<FjSkelJoint>& joints) {
    Skeleton skel;
    skel.joints_ = joints;
    skel.skeleton_ = build_ozz_skeleton(skel.joints_);
    skel.inverse_bind_matrices_ = compute_inverse_bind_matrices(skel.joints_);
    return skel;
}

void Skeleton::save(const std::string& path) const {
    nlohmann::json j;
    j["joints"] = nlohmann::json::array();

    for (const auto& joint : joints_) {
        nlohmann::json jj;
        jj["name"] = joint.name;
        jj["parent"] = joint.parent;
        jj["translation"] = {joint.translation.x, joint.translation.y, joint.translation.z};
        // JSON stores [x, y, z, w]
        jj["rotation"] = {joint.rotation.x, joint.rotation.y, joint.rotation.z, joint.rotation.w};
        jj["scale"] = {joint.scale.x, joint.scale.y, joint.scale.z};
        j["joints"].push_back(jj);
    }

    j["slots"] = nlohmann::json::array();
    for (const auto& slot : slots_) {
        nlohmann::json js;
        js["name"] = slot.name;
        js["bone_index"] = slot.bone_index;
        js["offset_translation"] = {slot.offset_translation.x, slot.offset_translation.y, slot.offset_translation.z};
        js["offset_rotation"] = {slot.offset_rotation.x, slot.offset_rotation.y, slot.offset_rotation.z, slot.offset_rotation.w};
        js["offset_scale"] = {slot.offset_scale.x, slot.offset_scale.y, slot.offset_scale.z};
        j["slots"].push_back(js);
    }

    std::ofstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to save skeleton: " + path);
    }
    file << j.dump(2);
    FJELL_CORE_INFO("Saved skeleton: {}", path);
}

int Skeleton::find_joint(const std::string& name) const {
    const auto& names = skeleton_.joint_names();
    for (int i = 0; i < skeleton_.num_joints(); ++i) {
        if (names[i] == name) {
            return i;
        }
    }
    return -1;
}

} // namespace fjell
