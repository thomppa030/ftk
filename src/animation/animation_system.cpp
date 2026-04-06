#include "animation/animation_system.hpp"
#include "animation/animator_component.hpp"
#include "core/profiler.hpp"
#include "renderer/components/skeletal_mesh_component.hpp"
#include "renderer/resources/skeletal_model.hpp"
#include "scene/scene.hpp"

namespace fjell {

AnimationSystem::AnimationSystem(VkDevice device, VmaAllocator allocator)
    : bone_buffer_{device, allocator} {
}

void AnimationSystem::update(Scene& scene, float dt) {
    FJELL_PROFILE_SCOPE_N("anim_system_update");
    for (auto& obj : scene.objects()) {
        auto* anim = obj->components.get<AnimatorComponent>();
        if (!anim) continue;

        // Auto-init from SkeletalModel's embedded skeleton
        if (!anim->initialized()) {
            auto* smc = obj->components.get<SkeletalMeshComponent>();
            if (smc && smc->skeletal_model && smc->skeletal_model->skeleton()) {
                anim->init(smc->skeletal_model->skeleton());
            }
        }

        if (anim->initialized()) {
            anim->set_owner(obj->as_spatial());
            anim->update(dt);
        }
    }
}

void AnimationSystem::upload_bones(Scene& scene, uint32_t frame_index) {
    FJELL_PROFILE_SCOPE_N("anim_upload_bones");
    bone_buffer_.begin_frame(frame_index);

    // Clear only previously active entries (O(active) instead of O(max_id))
    for (uint32_t id : active_ids_) {
        bone_offsets_[id] = 0xFFFFFFFF;
        bone_counts_[id] = 0;
    }
    active_ids_.clear();
    bone_entry_count_ = 0;

    for (auto& obj : scene.objects()) {
        auto* smc = obj->components.get<SkeletalMeshComponent>();
        if (!smc || !smc->skeletal_model) continue;

        const auto* skel = smc->skeletal_model->skeleton();
        if (!skel) continue;

        uint32_t num_joints = static_cast<uint32_t>(skel->num_joints());
        if (num_joints == 0) continue;

        uint32_t id = obj->id;
        if (id >= bone_offsets_.size()) {
            bone_offsets_.resize(id + 1, 0xFFFFFFFF);
            bone_counts_.resize(id + 1, 0);
        }

        // Use animated pose if AnimatorComponent exists and has a valid pose
        auto* anim = obj->components.get<AnimatorComponent>();
        if (anim && anim->initialized() && anim->has_active_clip() && anim->bone_count() > 0) {
            uint32_t offset = bone_buffer_.upload(anim->skin_matrices(), frame_index);
            bone_offsets_[id] = offset;
            bone_counts_[id] = anim->bone_count();
        } else {
            // Bind pose: identity matrices (mesh renders in rest position)
            if (bind_pose_cache_.size() < num_joints) {
                bind_pose_cache_.resize(num_joints, glm::mat4(1.0f));
            }
            uint32_t offset = bone_buffer_.upload(bind_pose_cache_, frame_index);
            bone_offsets_[id] = offset;
            bone_counts_[id] = num_joints;
        }

        active_ids_.push_back(id);
        bone_entry_count_++;
    }
}

uint32_t AnimationSystem::bone_offset_for(uint32_t node_id) const {
    if (node_id >= bone_offsets_.size()) return 0xFFFFFFFF;
    return bone_offsets_[node_id];
}

uint32_t AnimationSystem::bone_count_for(uint32_t node_id) const {
    if (node_id >= bone_counts_.size()) return 0;
    return bone_counts_[node_id];
}

} // namespace fjell
