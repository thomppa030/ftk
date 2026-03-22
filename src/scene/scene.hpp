#pragma once

#include "core/result.hpp"
#include "renderer/resource_handle.hpp"
#include "renderer/uniform_types.hpp"
#include "scene/node.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fjell {

class Project;
class ResourceManager;

class Scene {
public:
    uint32_t add(ModelHandle model_handle, MaterialHandle material_handle,
                 ResourceManager& resources,
                 Transform transform = {},
                 const std::string& name = "Object",
                 const std::string& source = "",
                 const std::string& material_name = "default");
    uint32_t add_empty(Transform transform = {},
                       const std::string& name = "Empty");
    uint32_t add_folder(const std::string& name = "Folder");
    uint32_t add_node(std::unique_ptr<Node> node);
    void remove(uint32_t id);
    [[nodiscard]] std::unique_ptr<Node> extract(uint32_t id);
    void restore(std::unique_ptr<Node> node);
    void clear();

    void compute_world_matrices(class ThreadPool* pool = nullptr);
    void reparent(uint32_t child_id, int32_t new_parent_id);

    [[nodiscard]] Node* find(uint32_t id);
    [[nodiscard]] const Node* find(uint32_t id) const;
    [[nodiscard]] Node3D* find_spatial(uint32_t id) {
        auto* n = find(id);
        return n ? n->as_spatial() : nullptr;
    }
    [[nodiscard]] const Node3D* find_spatial(uint32_t id) const {
        auto* n = find(id);
        return n ? n->as_spatial() : nullptr;
    }
    [[nodiscard]] int index_of(uint32_t id) const;

    Result<> save(const std::string& path);

    void mark_dirty() { dirty_ = true; }
    [[nodiscard]] bool is_dirty() const { return dirty_; }
    static Result<Scene> load(const std::string& path,
                              ResourceManager& resources,
                              const Project* project = nullptr);

    [[nodiscard]] int object_count() const { return static_cast<int>(objects_.size()); }
    [[nodiscard]] auto& objects() { return objects_; }
    [[nodiscard]] const auto& objects() const { return objects_; }

    void rebuild_id_index();

    [[nodiscard]] uint32_t next_id() const { return next_id_; }
    void set_next_id(uint32_t id) { next_id_ = id; }

    // Point/spot lights
    [[nodiscard]] std::vector<PointLight>& point_lights() { return point_lights_; }
    [[nodiscard]] const std::vector<PointLight>& point_lights() const { return point_lights_; }
    [[nodiscard]] std::vector<SpotLight>& spot_lights() { return spot_lights_; }
    [[nodiscard]] const std::vector<SpotLight>& spot_lights() const { return spot_lights_; }

    void add_point_light(const PointLight& light = {}) { point_lights_.push_back(light); mark_dirty(); }
    void add_spot_light(const SpotLight& light = {}) { spot_lights_.push_back(light); mark_dirty(); }
    void insert_point_light(int index, const PointLight& light);
    void insert_spot_light(int index, const SpotLight& light);
    void remove_point_light(int index);
    void remove_spot_light(int index);

private:
    std::vector<std::unique_ptr<Node>> objects_;
    std::unordered_map<uint32_t, int> id_to_index_;
    std::vector<PointLight> point_lights_;
    std::vector<SpotLight> spot_lights_;
    uint32_t next_id_{1};
    bool dirty_{false};
    bool index_dirty_{true};
    std::vector<uint8_t> computed_buf_; // reusable per-frame scratch
};

} // namespace fjell
