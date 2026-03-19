#pragma once

#include "audio/audio_component.hpp"
#include "physics/physics_types.hpp"
#include "renderer/particle_emitter.hpp"
#include "renderer/sprite_desc.hpp"
#include "renderer/resource_handle.hpp"
#include "renderer/uniform_types.hpp"
#include "scene/node_component.hpp"
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
class ScriptComponent;
class Model;
class Material;
class Project;
class ResourceManager;

// ── Script entry ────────────────────────────────────────────────────────

struct ScriptEntry {
    std::unique_ptr<ScriptComponent> instance;
    std::string type_name;
    nlohmann::json serialized;

    ScriptEntry();
    ~ScriptEntry();
    ScriptEntry(ScriptEntry&&) noexcept;
    ScriptEntry& operator=(ScriptEntry&&) noexcept;
};

// ── Mesh component ──────────────────────────────────────────────────────

struct MeshComponent : NodeComponent {
    ModelHandle model_handle;
    MaterialHandle material_handle;
    Model* model{nullptr};
    Material* material{nullptr};
    std::string material_name;
};

// ── Scene node ──────────────────────────────────────────────────────────

struct SceneNode {
    uint32_t id{0};
    int32_t parent_id{-1};
    glm::mat4 world_matrix_{1.0f};

    Transform transform;
    std::string name;
    std::string source;

    // Components — built-in (typed access) + script (polymorphic)
    std::optional<MeshComponent> mesh;
    std::optional<RigidbodyComponent> rigidbody;
    std::optional<ColliderComponent> collider;
    std::optional<EmitterComponent> emitter;
    std::optional<SpriteComponent> sprite;
    std::optional<AudioSourceComponent> audio;

    // Scripting
    std::unique_ptr<Actor> actor;
    std::string actor_type_name;
    std::vector<ScriptEntry> scripts;

    SceneNode();
    virtual ~SceneNode();
    SceneNode(SceneNode&&) noexcept;
    SceneNode& operator=(SceneNode&&) noexcept;

    // Deep clone for snapshotting — copies flat data, leaves unique_ptrs null
    // Virtual so derived node types (LightNode, etc.) can clone their extra fields
    [[nodiscard]] virtual std::unique_ptr<SceneNode> clone() const;

protected:
    // Copy base fields into a derived node's clone
    void clone_base_into(SceneNode& target) const;
};

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
    uint32_t add_node(std::unique_ptr<SceneNode> node);
    void remove(uint32_t id);
    [[nodiscard]] std::unique_ptr<SceneNode> extract(uint32_t id);
    void restore(std::unique_ptr<SceneNode> node);
    void clear();

    void compute_world_matrices();
    void reparent(uint32_t child_id, int32_t new_parent_id);

    [[nodiscard]] SceneNode* find(uint32_t id);
    [[nodiscard]] const SceneNode* find(uint32_t id) const;
    [[nodiscard]] int index_of(uint32_t id) const;

    void save(const std::string& path);

    void mark_dirty() { dirty_ = true; }
    [[nodiscard]] bool is_dirty() const { return dirty_; }
    static Scene load(const std::string& path,
                      ResourceManager& resources,
                      const Project* project = nullptr);

    [[nodiscard]] int object_count() const { return static_cast<int>(objects_.size()); }
    [[nodiscard]] auto& objects() { return objects_; }
    [[nodiscard]] const auto& objects() const { return objects_; }

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
    std::vector<std::unique_ptr<SceneNode>> objects_;
    std::vector<PointLight> point_lights_;
    std::vector<SpotLight> spot_lights_;
    uint32_t next_id_{1};
    bool dirty_{false};
};

} // namespace fjell
