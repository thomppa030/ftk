#pragma once

#include "animation/animator_component.hpp"
#include "animation/bone_buffer.hpp"

#include <cstdint>
#include <memory>
#include <unordered_map>

namespace fjell {

struct SceneNode;

// Central animation system. Updates all animators and uploads bone matrices
// to the GPU each frame. Owns the BoneBuffer SSBO.
class AnimationSystem {
public:
    AnimationSystem(VkDevice device, VkPhysicalDevice physical_device);

    // Register/remove animators for scene nodes.
    AnimatorComponent& add_animator(uint32_t node_id);
    void remove_animator(uint32_t node_id);
    [[nodiscard]] AnimatorComponent* animator(uint32_t node_id);
    [[nodiscard]] const AnimatorComponent* animator(uint32_t node_id) const;

    // Update all animators (call once per frame, before rendering).
    void update(float dt);

    // Upload all bone matrices to the GPU SSBO (call after update, before draw).
    void upload_bones(uint32_t frame_index);

    // Query the bone SSBO offset for a node (used by IndirectBatch to fill GpuDrawData).
    // Returns 0xFFFFFFFF if the node has no animator.
    [[nodiscard]] uint32_t bone_offset_for(uint32_t node_id) const;
    [[nodiscard]] uint32_t bone_count_for(uint32_t node_id) const;

    // BoneBuffer access for descriptor set updates.
    [[nodiscard]] BoneBuffer& bone_buffer() { return bone_buffer_; }
    [[nodiscard]] const BoneBuffer& bone_buffer() const { return bone_buffer_; }

    [[nodiscard]] bool has_animated_nodes() const { return !animators_.empty(); }

private:
    BoneBuffer bone_buffer_;
    std::unordered_map<uint32_t, std::unique_ptr<AnimatorComponent>> animators_;
    std::unordered_map<uint32_t, uint32_t> bone_offsets_; // node_id → SSBO offset (per frame)
};

} // namespace fjell
