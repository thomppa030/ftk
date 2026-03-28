#include "animation/animation_system.hpp"
#include "animation/animator_component.hpp"
#include "core/profiler.hpp"
#include "renderer/skeletal_mesh_component.hpp"
#include "scene/scene.hpp"

namespace fjell {

AnimationSystem::AnimationSystem(VkDevice device, VkPhysicalDevice physical_device)
    : bone_buffer_{device, physical_device} {
}

void AnimationSystem::update(Scene& scene, float dt) {
    FJELL_PROFILE_SCOPE_N("anim_system_update");
    for (auto& obj : scene.objects()) {
        auto* skel = obj->components.get<SkeletalMeshComponent>();
        if (!skel || !skel->animator) continue;
        auto& anim = *skel->animator;
        if (anim.initialized()) {
            anim.update(dt);
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
        auto* skel = obj->components.get<SkeletalMeshComponent>();
        if (!skel || !skel->animator) continue;
        auto& anim = *skel->animator;
        if (!anim.initialized() || anim.bone_count() == 0) continue;

        uint32_t id = obj->id;
        // Grow flat arrays on demand
        if (id >= bone_offsets_.size()) {
            bone_offsets_.resize(id + 1, 0xFFFFFFFF);
            bone_counts_.resize(id + 1, 0);
        }

        uint32_t offset = bone_buffer_.upload(anim.skin_matrices(), frame_index);
        bone_offsets_[id] = offset;
        bone_counts_[id] = anim.bone_count();
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
