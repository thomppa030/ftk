#include "scene/scene.hpp"
#include "renderer/material.hpp"
#include "renderer/model.hpp"

namespace fjell {

uint32_t Scene::add(Model* model, Material* material, Transform transform) {
    objects_.push_back({model, material, transform});
    return static_cast<uint32_t>(objects_.size() - 1);
}

void Scene::remove(uint32_t id) {
    if (id < objects_.size()) {
        objects_.erase(objects_.begin() + id);
    }
}

void Scene::render(VkCommandBuffer cmd, VkPipelineLayout pipeline_layout) const {
    for (const auto& obj : objects_) {
        glm::mat4 model_matrix = obj.transform.matrix();
        vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(glm::mat4), &model_matrix);

        if (obj.material) {
            obj.material->bind(cmd, pipeline_layout);
        }

        obj.model->draw(cmd);
    }
}

} // namespace fjell
