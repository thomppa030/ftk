#pragma once

#include "scene/transform.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace fjell {

class Model;
class Material;

struct RenderObject {
    Model* model;
    Material* material;
    Transform transform;
};

class Scene {
public:
    uint32_t add(Model* model, Material* material, Transform transform = {});
    void remove(uint32_t id);

    void render(VkCommandBuffer cmd, VkPipelineLayout pipeline_layout) const;

private:
    std::vector<RenderObject> objects_;
};

} // namespace fjell
