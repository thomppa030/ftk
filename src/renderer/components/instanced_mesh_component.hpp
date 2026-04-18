#pragma once

#include "renderer/frustum.hpp"
#include "renderer/resources/resource_handle.hpp"
#include "scene/node_component.hpp"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace fjell {

class Model;
class Material;

struct InstancedMeshComponent : NodeComponent {
    ModelHandle model_handle;
    MaterialHandle material_handle;
    Model* model{nullptr};
    Material* material{nullptr};
    std::string material_name;

    std::vector<glm::mat4> instances;

    // Union of every instance's model-local-AABB transformed by its local-space
    // instance matrix, expressed in the component's local (node-parent) space.
    // Cached across frames; recomputed when `dirty` is set by editor mutations
    // or when the model changes.
    AABB local_bounds{};
    bool dirty{true};
};

} // namespace fjell
