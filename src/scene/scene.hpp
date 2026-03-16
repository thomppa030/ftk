#pragma once

#include "scene/transform.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace fjell {

class Model;
class Material;
class Project;
struct RenderStats;

struct ComponentInstance {
    uint32_t type_id{0};
    std::string type_name;
    std::vector<uint8_t> data;
    nlohmann::json serialized;
};

struct RenderObject {
    uint32_t id{0};
    int32_t parent_id{-1};
    glm::mat4 world_matrix_{1.0f};

    Model* model;
    Material* material;
    Transform transform;
    std::string name;
    std::string source;       // "primitive:cube", "primitive:plane", etc. or model file path
    std::string material_name;

    std::vector<ComponentInstance> components;
};

class Scene {
public:
    uint32_t add(Model* model, Material* material, Transform transform = {},
                 const std::string& name = "Object",
                 const std::string& source = "",
                 const std::string& material_name = "default");
    void remove(uint32_t id);
    void clear();

    void compute_world_matrices();
    void reparent(uint32_t child_id, int32_t new_parent_id);

    [[nodiscard]] RenderObject* find(uint32_t id);
    [[nodiscard]] const RenderObject* find(uint32_t id) const;
    [[nodiscard]] int index_of(uint32_t id) const;

    void save(const std::string& path) const;
    static Scene load(const std::string& path,
                      class ResourceManager& resources,
                      const Project* project = nullptr);

    void render(VkCommandBuffer cmd, VkPipelineLayout pipeline_layout,
                RenderStats* stats = nullptr) const;

    [[nodiscard]] int object_count() const { return static_cast<int>(objects_.size()); }
    [[nodiscard]] std::vector<RenderObject>& objects() { return objects_; }
    [[nodiscard]] const std::vector<RenderObject>& objects() const { return objects_; }

    [[nodiscard]] uint32_t next_id() const { return next_id_; }
    void set_next_id(uint32_t id) { next_id_ = id; }

private:
    std::vector<RenderObject> objects_;
    uint32_t next_id_{1};
};

} // namespace fjell
