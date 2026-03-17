#pragma once

#include "scripting/fjell_api.h"
#include "scripting/fjell_registry.hpp"
#include "scripting/fjell_world.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fjell {

class Actor;
class Component;
class Scene;
class Input;
class Clock;
class PhysicsWorld;
struct RenderObject;

// Engine-owned deep copy of a component type from the .so
struct OwnedFieldDesc {
    std::string name;
    FjellFieldType type;
    uint32_t offset;
    uint32_t size;
    std::vector<std::string> enum_labels;
    float min;
    float max;
    float speed;
};

// Old-style C API component type (kept during migration)
struct OwnedComponentType {
    std::string name;
    uint32_t type_id;
    uint32_t instance_size;
    uint32_t version;

    FjellCreateFn create;
    FjellDestroyFn destroy;
    FjellUpdateFn update;
    FjellInspectorFn inspector;
    FjellSerializeFn serialize;
    FjellDeserializeFn deserialize;

    std::vector<OwnedFieldDesc> fields;
    bool loaded{true};
};

// New OOP actor type
struct OwnedActorType {
    std::string name;
    uint32_t type_id;
    Actor* (*create)();
    std::vector<OwnedFieldDesc> fields;
    bool loaded{true};
};

// New OOP component type
struct OwnedOOPComponentType {
    std::string name;
    uint32_t type_id;
    Component* (*create)();
    std::vector<OwnedFieldDesc> fields;
    bool loaded{true};
};

class ScriptManager {
public:
    ScriptManager();
    ~ScriptManager();

    ScriptManager(const ScriptManager&) = delete;
    ScriptManager& operator=(const ScriptManager&) = delete;
    ScriptManager(ScriptManager&&) = delete;
    ScriptManager& operator=(ScriptManager&&) = delete;

    bool load(const std::filesystem::path& so_path);
    void unload(Scene& scene);
    bool reload(Scene& scene, const std::filesystem::path& so_path);

    // Populate engine services (World, API) without ticking — call before begin_play
    void prepare(Scene& scene, const Input& input, const Clock& clock,
                 PhysicsWorld* physics = nullptr);

    void update(Scene& scene, const Input& input, const Clock& clock,
                PhysicsWorld* physics = nullptr);

    // Old-style C API components
    bool add_component(RenderObject& obj, const std::string& type_name);
    void remove_component(RenderObject& obj, uint32_t type_id);
    void draw_inspector(RenderObject& obj);

    // New OOP components
    bool add_oop_component(RenderObject& obj, const std::string& type_name);
    void remove_oop_component(RenderObject& obj, const std::string& type_name);
    void draw_oop_inspector(RenderObject& obj);

    void serialize_all(Scene& scene);
    void deserialize_all(Scene& scene);

    // Old-style type queries
    [[nodiscard]] const OwnedComponentType* find_type(uint32_t type_id) const;
    [[nodiscard]] const OwnedComponentType* find_type(const std::string& name) const;
    [[nodiscard]] const std::vector<OwnedComponentType>& types() const { return type_list_; }

    // New OOP type queries
    [[nodiscard]] const OwnedActorType* find_actor_type(const std::string& name) const;
    [[nodiscard]] const OwnedOOPComponentType* find_oop_component_type(const std::string& name) const;
    [[nodiscard]] const std::vector<OwnedActorType>& actor_types() const { return actor_type_list_; }
    [[nodiscard]] const std::vector<OwnedOOPComponentType>& oop_component_types() const { return oop_type_list_; }

    [[nodiscard]] bool is_loaded() const { return handle_ != nullptr; }
    [[nodiscard]] static uint32_t hash_type_name(const std::string& name);

    // Called by begin_play/end_play on actors/components
    void call_begin_play(Scene& scene);
    void call_end_play(Scene& scene);

#ifndef NDEBUG
    [[nodiscard]] bool poll_reload(Scene& scene);
    void watch(const std::filesystem::path& so_path);
#endif

private:
    void populate_engine_api(Scene& scene, const Input& input, const Clock& clock,
                             PhysicsWorld* physics);
    void build_registry();

    // Auto-serialize/deserialize using field descriptors on a raw pointer
    void auto_serialize_fields(const std::vector<OwnedFieldDesc>& fields,
                               const void* base, nlohmann::json& out) const;
    void auto_deserialize_fields(const std::vector<OwnedFieldDesc>& fields,
                                 void* base, const nlohmann::json& in) const;

    // Legacy overloads (delegate to field-based versions)
    void auto_serialize(const OwnedComponentType& type, const void* data,
                        nlohmann::json& out) const;
    void auto_deserialize(const OwnedComponentType& type, void* data,
                          const nlohmann::json& in) const;

    void* handle_{nullptr};
    std::filesystem::path so_path_;
    std::filesystem::path loaded_temp_path_;

    // Old-style C API types
    std::unordered_map<uint32_t, std::size_t> type_map_;
    std::vector<OwnedComponentType> type_list_;

    // New OOP types
    std::unordered_map<uint32_t, std::size_t> actor_type_map_;
    std::vector<OwnedActorType> actor_type_list_;
    std::unordered_map<uint32_t, std::size_t> oop_type_map_;
    std::vector<OwnedOOPComponentType> oop_type_list_;

    FjellEngineApi api_{};
    World world_{};
    Scene* current_scene_{nullptr};

#ifndef NDEBUG
    int inotify_fd_{-1};
    int watch_fd_{-1};
#endif
};

} // namespace fjell
