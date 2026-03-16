#pragma once

#include "scripting/fjell_api.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace fjell {

class Scene;
class Input;
class Clock;
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
    bool loaded{true}; // false after unload — function pointers invalid
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

    void update(Scene& scene, const Input& input, const Clock& clock);
    bool add_component(RenderObject& obj, const std::string& type_name);
    void remove_component(RenderObject& obj, uint32_t type_id);
    void draw_inspector(RenderObject& obj);

    void serialize_all(Scene& scene);
    void deserialize_all(Scene& scene);

    [[nodiscard]] const OwnedComponentType* find_type(uint32_t type_id) const;
    [[nodiscard]] const OwnedComponentType* find_type(const std::string& name) const;
    [[nodiscard]] const std::vector<OwnedComponentType>& types() const { return type_list_; }
    [[nodiscard]] bool is_loaded() const { return handle_ != nullptr; }

    [[nodiscard]] static uint32_t hash_type_name(const std::string& name);

#ifndef NDEBUG
    [[nodiscard]] bool poll_reload(Scene& scene);
    void watch(const std::filesystem::path& so_path);
#endif

private:
    void populate_engine_api(Scene& scene, const Input& input, const Clock& clock);
    void build_registry();

    void auto_serialize(const OwnedComponentType& type, const void* data,
                        nlohmann::json& out) const;
    void auto_deserialize(const OwnedComponentType& type, void* data,
                          const nlohmann::json& in) const;

    void* handle_{nullptr};
    std::filesystem::path so_path_;
    std::filesystem::path loaded_temp_path_;

    std::unordered_map<uint32_t, std::size_t> type_map_; // type_id -> index in type_list_
    std::vector<OwnedComponentType> type_list_;

    FjellEngineApi api_{};
    Scene* current_scene_{nullptr}; // set during populate_engine_api, used by callbacks

#ifndef NDEBUG
    int inotify_fd_{-1};
    int watch_fd_{-1};
#endif
};

} // namespace fjell
