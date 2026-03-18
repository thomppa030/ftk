#pragma once

#include "physics/physics_types.hpp"
#include "renderer/particle_emitter.hpp"
#include "renderer/resource_handle.hpp"
#include "renderer/uniform_types.hpp"
#include "scene/transform.hpp"

#include <glm/glm.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fjell {

class Actor;
class Component;
class Model;
class Material;
class Project;
class ResourceManager;

// ── Component entry ─────────────────────────────────────────────────────

struct ComponentEntry {
    std::unique_ptr<Component> instance;
    std::string type_name;
    nlohmann::json serialized;

    ComponentEntry();
    ~ComponentEntry();
    ComponentEntry(ComponentEntry&&) noexcept;
    ComponentEntry& operator=(ComponentEntry&&) noexcept;
};

// ── Scene object ────────────────────────────────────────────────────────

struct RenderObject {
    uint32_t id{0};
    int32_t parent_id{-1};
    glm::mat4 world_matrix_{1.0f};

    ModelHandle model_handle;
    MaterialHandle material_handle;
    Model* model{nullptr};
    Material* material{nullptr};
    Transform transform;
    std::string name;
    std::string source;
    std::string material_name;

    // Actor/component system
    std::unique_ptr<Actor> actor;
    std::string actor_type_name;
    std::vector<ComponentEntry> components;

    std::optional<RigidbodyDesc> rigidbody;
    std::optional<ColliderDesc> collider;
    std::optional<ParticleEmitter> emitter;

    RenderObject();
    ~RenderObject();
    RenderObject(RenderObject&&) noexcept;
    RenderObject& operator=(RenderObject&&) noexcept;

    // Deep clone for snapshotting — copies flat data, leaves unique_ptrs null
    [[nodiscard]] RenderObject clone_flat() const {
        RenderObject copy{};
        copy.id = id;
        copy.parent_id = parent_id;
        copy.world_matrix_ = world_matrix_;
        copy.model_handle = model_handle;
        copy.material_handle = material_handle;
        copy.model = model;
        copy.material = material;
        copy.transform = transform;
        copy.name = name;
        copy.source = source;
        copy.material_name = material_name;
        copy.actor_type_name = actor_type_name;
        for (const auto& comp : components) {
            ComponentEntry entry;
            entry.type_name = comp.type_name;
            entry.serialized = comp.serialized;
            copy.components.push_back(std::move(entry));
        }
        copy.rigidbody = rigidbody;
        copy.collider = collider;
        copy.emitter = emitter;
        return copy;
    }
};

class Scene {
public:
    uint32_t add(ModelHandle model_handle, MaterialHandle material_handle,
                 ResourceManager& resources,
                 Transform transform = {},
                 const std::string& name = "Object",
                 const std::string& source = "",
                 const std::string& material_name = "default");
    void remove(uint32_t id);
    [[nodiscard]] RenderObject extract(uint32_t id);
    void restore(RenderObject obj);
    void clear();

    void compute_world_matrices();
    void reparent(uint32_t child_id, int32_t new_parent_id);

    [[nodiscard]] RenderObject* find(uint32_t id);
    [[nodiscard]] const RenderObject* find(uint32_t id) const;
    [[nodiscard]] int index_of(uint32_t id) const;

    void save(const std::string& path);

    void mark_dirty() { dirty_ = true; }
    [[nodiscard]] bool is_dirty() const { return dirty_; }
    static Scene load(const std::string& path,
                      ResourceManager& resources,
                      const Project* project = nullptr);

    [[nodiscard]] int object_count() const { return static_cast<int>(objects_.size()); }
    [[nodiscard]] std::vector<RenderObject>& objects() { return objects_; }
    [[nodiscard]] const std::vector<RenderObject>& objects() const { return objects_; }

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
    std::vector<RenderObject> objects_;
    std::vector<PointLight> point_lights_;
    std::vector<SpotLight> spot_lights_;
    uint32_t next_id_{1};
    bool dirty_{false};
};

} // namespace fjell
