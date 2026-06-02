#pragma once

#include "core/result.hpp"
#include "renderer/resources/resource_handle.hpp"
#include "renderer/uniform_types.hpp"
#include "scene/node.hpp"

#include <cstdint>
#include <memory>
#include <span>
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
    [[nodiscard]] std::vector<const Node*> collect_subtree(uint32_t root_id) const;

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

    // Generation tracking for safe weak references
    [[nodiscard]] uint32_t generation_of(uint32_t id) const;

    // Deferred destruction for safe mid-tick removal
    void queue_destroy(uint32_t id);
    void flush_destroyed();
    [[nodiscard]] std::span<const uint32_t> pending_destroy() const { return pending_destroy_; }

    // Mid-tick mutation guard. While locked (e.g. during the script tick loop),
    // add_node() defers insertion into a pending queue instead of mutating
    // objects_, which would invalidate the iterator the caller is walking.
    // flush_spawned() materializes the queue afterward. RAII helper: TickLock.
    void lock_iteration()   { ++iteration_depth_; }
    void unlock_iteration() { if (iteration_depth_ > 0) --iteration_depth_; }
    [[nodiscard]] bool iterating() const { return iteration_depth_ > 0; }
    void flush_spawned();
    [[nodiscard]] bool has_pending_spawns() const { return !pending_spawn_.empty(); }

    struct TickLock {
        explicit TickLock(Scene& s) : scene_(s) { scene_.lock_iteration(); }
        ~TickLock() { scene_.unlock_iteration(); }
        TickLock(const TickLock&) = delete;
        TickLock& operator=(const TickLock&) = delete;
        Scene& scene_;
    };

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
    std::unordered_map<uint32_t, uint32_t> id_generation_;
    std::vector<uint32_t> pending_destroy_;

    int iteration_depth_{0};                              // >0 while a tick walks objects_
    std::vector<std::unique_ptr<Node>> pending_spawn_;    // deferred adds, flushed post-tick
};

} // namespace fjell
