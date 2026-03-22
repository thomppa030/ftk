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
    bone_offsets_.clear();
    bone_counts_.clear();

    for (auto& obj : scene.objects()) {
        auto* skel = obj->components.get<SkeletalMeshComponent>();
        if (!skel || !skel->animator) continue;
        auto& anim = *skel->animator;
        if (!anim.initialized() || anim.bone_count() == 0) continue;

        uint32_t offset = bone_buffer_.upload(anim.skin_matrices(), frame_index);
        bone_offsets_[obj->id] = offset;
        bone_counts_[obj->id] = anim.bone_count();
    }
}

uint32_t AnimationSystem::bone_offset_for(uint32_t node_id) const {
    auto it = bone_offsets_.find(node_id);
    return it != bone_offsets_.end() ? it->second : 0xFFFFFFFF;
}

uint32_t AnimationSystem::bone_count_for(uint32_t node_id) const {
    auto it = bone_counts_.find(node_id);
    return it != bone_counts_.end() ? it->second : 0;
}

} // namespace fjell
