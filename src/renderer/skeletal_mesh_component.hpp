#pragma once

#include "renderer/resource_handle.hpp"
#include "scene/node_component.hpp"

#include <memory>
#include <string>
#include <vector>

namespace fjell {

class AnimatorComponent;
class Material;
class SkeletalModel;
class Skeleton;
class AnimationClip;

struct SkeletalMeshComponent : NodeComponent {
    SkeletalModelHandle skeletal_model_handle;
    MaterialHandle material_handle;
    SkeletalModel* skeletal_model{nullptr};
    Material* material{nullptr};
    std::string material_name;
    std::unique_ptr<AnimatorComponent> animator;

    // Owned animation data (skeleton + clips stay alive as long as the component)
    std::shared_ptr<Skeleton> skeleton;
    std::vector<std::shared_ptr<AnimationClip>> clips;

    // Asset paths for serialization
    std::string skeleton_path;
    std::vector<std::string> clip_paths;
    std::string animset_path;

    SkeletalMeshComponent();
    ~SkeletalMeshComponent();
    SkeletalMeshComponent(SkeletalMeshComponent&&) noexcept;
    SkeletalMeshComponent& operator=(SkeletalMeshComponent&&) noexcept;
};

} // namespace fjell
