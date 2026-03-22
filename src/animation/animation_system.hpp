#pragma once

#include "animation/bone_buffer.hpp"

#include <cstdint>
#include <unordered_map>

namespace fjell {

class Scene;

// Central animation system. Iterates scene nodes with SkeletalMeshComponent,
// ticks their animators, and uploads bone matrices to the GPU each frame.
class AnimationSystem {
public:
    AnimationSystem(VkDevice device, VkPhysicalDevice physical_device);

    // Update all animators on skeletal mesh nodes (call once per frame).
    void update(Scene& scene, float dt);

    // Upload all bone matrices to the GPU SSBO (call after update, before draw).
    void upload_bones(Scene& scene, uint32_t frame_index);

    // Query the bone SSBO offset for a node (used by draw data).
    // Returns 0xFFFFFFFF if the node has no animated skeleton.
    [[nodiscard]] uint32_t bone_offset_for(uint32_t node_id) const;
    [[nodiscard]] uint32_t bone_count_for(uint32_t node_id) const;

    [[nodiscard]] BoneBuffer& bone_buffer() { return bone_buffer_; }
    [[nodiscard]] const BoneBuffer& bone_buffer() const { return bone_buffer_; }

    [[nodiscard]] bool has_bones_this_frame() const { return !bone_offsets_.empty(); }

private:
    BoneBuffer bone_buffer_;
    std::unordered_map<uint32_t, uint32_t> bone_offsets_;  // node_id → SSBO offset
    std::unordered_map<uint32_t, uint32_t> bone_counts_;   // node_id → joint count
};

} // namespace fjell
