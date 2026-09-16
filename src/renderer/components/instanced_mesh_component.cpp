#include "renderer/components/instanced_mesh_component.hpp"

#include <glm/gtc/matrix_transform.hpp>

namespace fjell {

glm::mat4 InstancePlacement::matrix() const {
    return glm::translate(glm::mat4(1.0f), position)
         * glm::mat4_cast(rotation)
         * glm::scale(glm::mat4(1.0f), scale);
}

void InstancedMeshComponent::rebuild_instances() {
    instances.clear();
    instances.reserve(placements.size());
    for (const auto& placement : placements) {
        instances.push_back(placement.matrix());
    }
}

} // namespace fjell
