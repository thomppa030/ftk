#pragma once

#include "scene/transform.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

namespace fjell {

class Model;
class Material;

struct RenderObject {
    Model* model;
    Material* material;
    Transform transform;
    std::string name;
};

class Scene {
public:
    uint32_t add(Model* model, Material* material, Transform transform = {},
                 const std::string& name = "Object");
    void remove(uint32_t id);
    void clear();

    void render(VkCommandBuffer cmd, VkPipelineLayout pipeline_layout) const;

    [[nodiscard]] int object_count() const { return static_cast<int>(objects_.size()); }
    [[nodiscard]] std::vector<RenderObject>& objects() { return objects_; }
    [[nodiscard]] const std::vector<RenderObject>& objects() const { return objects_; }

private:
    std::vector<RenderObject> objects_;
};

} // namespace fjell
