#pragma once

#include "renderer/resource_handle.hpp"
#include "scene/node_component.hpp"

#include <string>

namespace fjell {

class Model;
class Material;

struct MeshComponent : NodeComponent {
    ModelHandle model_handle;
    MaterialHandle material_handle;
    Model* model{nullptr};
    Material* material{nullptr};
    std::string material_name;
};

} // namespace fjell
