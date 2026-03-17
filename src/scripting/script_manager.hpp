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

struct OwnedActorType {
    std::string name;
    uint32_t type_id;
    Actor* (*create)();
    std::vector<OwnedFieldDesc> fields;
    bool loaded{true};
};

struct OwnedComponentType {
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

    void prepare(Scene& scene, const Input& input, const Clock& clock,
                 PhysicsWorld* physics = nullptr);
    void update(Scene& scene, const Input& input, const Clock& clock,
                PhysicsWorld* physics = nullptr);

    bool add_component(RenderObject& obj, const std::string& type_name);
    void remove_component(RenderObject& obj, const std::string& type_name);
    void draw_inspector(RenderObject& obj);

    void serialize_all(Scene& scene);
    void deserialize_all(Scene& scene);

    void call_begin_play(Scene& scene);
    void call_end_play(Scene& scene);

    [[nodiscard]] const OwnedActorType* find_actor_type(const std::string& name) const;
    [[nodiscard]] const OwnedComponentType* find_component_type(const std::string& name) const;
    [[nodiscard]] const std::vector<OwnedActorType>& actor_types() const { return actor_type_list_; }
    [[nodiscard]] const std::vector<OwnedComponentType>& component_types() const { return component_type_list_; }

    [[nodiscard]] bool is_loaded() const { return handle_ != nullptr; }
    [[nodiscard]] static uint32_t hash_type_name(const std::string& name);

#ifndef NDEBUG
    [[nodiscard]] bool poll_reload(Scene& scene);
    void watch(const std::filesystem::path& so_path);
#endif

private:
    void populate_world(Scene& scene, const Input& input, const Clock& clock,
                        PhysicsWorld* physics);
    void build_registry();

    void auto_serialize_fields(const std::vector<OwnedFieldDesc>& fields,
                               const void* base, nlohmann::json& out) const;
    void auto_deserialize_fields(const std::vector<OwnedFieldDesc>& fields,
                                 void* base, const nlohmann::json& in) const;

    void* handle_{nullptr};
    std::filesystem::path so_path_;
    std::filesystem::path loaded_temp_path_;

    std::unordered_map<uint32_t, std::size_t> actor_type_map_;
    std::vector<OwnedActorType> actor_type_list_;
    std::unordered_map<uint32_t, std::size_t> component_type_map_;
    std::vector<OwnedComponentType> component_type_list_;

    World world_{};

#ifndef NDEBUG
    int inotify_fd_{-1};
    int watch_fd_{-1};
#endif
};

} // namespace fjell
