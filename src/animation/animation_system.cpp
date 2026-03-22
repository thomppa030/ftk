#include "animation/animation_system.hpp"

namespace fjell {

AnimationSystem::AnimationSystem(VkDevice device, VkPhysicalDevice physical_device)
    : bone_buffer_{device, physical_device} {
}

AnimatorComponent& AnimationSystem::add_animator(uint32_t node_id) {
    auto [it, inserted] = animators_.emplace(node_id, std::make_unique<AnimatorComponent>());
    return *it->second;
}

void AnimationSystem::remove_animator(uint32_t node_id) {
    animators_.erase(node_id);
    bone_offsets_.erase(node_id);
}

AnimatorComponent* AnimationSystem::animator(uint32_t node_id) {
    auto it = animators_.find(node_id);
    return it != animators_.end() ? it->second.get() : nullptr;
}

const AnimatorComponent* AnimationSystem::animator(uint32_t node_id) const {
    auto it = animators_.find(node_id);
    return it != animators_.end() ? it->second.get() : nullptr;
}

void AnimationSystem::update(float dt) {
    for (auto& [id, anim] : animators_) {
        if (anim->initialized()) {
            anim->update(dt);
        }
    }
}

void AnimationSystem::upload_bones(uint32_t frame_index) {
    bone_buffer_.begin_frame(frame_index);
    bone_offsets_.clear();

    for (auto& [node_id, anim] : animators_) {
        if (!anim->initialized() || anim->bone_count() == 0) continue;

        uint32_t offset = bone_buffer_.upload(anim->skin_matrices(), frame_index);
        bone_offsets_[node_id] = offset;
    }
}

uint32_t AnimationSystem::bone_offset_for(uint32_t node_id) const {
    auto it = bone_offsets_.find(node_id);
    return it != bone_offsets_.end() ? it->second : 0xFFFFFFFF;
}

uint32_t AnimationSystem::bone_count_for(uint32_t node_id) const {
    auto it = animators_.find(node_id);
    if (it == animators_.end() || !it->second->initialized()) return 0;
    return it->second->bone_count();
}

} // namespace fjell
