#pragma once

#include "renderer/resource_handle.hpp"
#include "scene/node_component.hpp"

#include <string>

namespace fjell {

class Material;
class SkeletalModel;

struct SkeletalMeshComponent : NodeComponent {
    SkeletalModelHandle skeletal_model_handle;
    MaterialHandle material_handle;
    SkeletalModel* skeletal_model{nullptr};
    Material* material{nullptr};
    std::string material_name;
};

} // namespace fjell
