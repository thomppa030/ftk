#pragma once

#include "core/result.hpp"
#include "scripting/fjell/api.h"
#include "scripting/fjell/registry.hpp"
#include "scripting/fjell/world.hpp"
#include "platform/shared_library.hpp"

#ifndef NDEBUG
#include "platform/file_watcher.hpp"
#endif

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace fjell {

class ScriptComponent;
class UiWidget;
using UiComponent = UiWidget; // backward compat
class Scene;
class Input;
class Clock;
class PhysicsWorld;
struct Node3D;

struct OwnedFieldDesc {
    std::string name;
    FjellFieldType type;
    uint32_t offset;
    uint32_t size;
    std::vector<std::string> enum_labels;
    float min;
    float max;
    float speed;
    std::string ref_type_name;  // for WEAK_REF/REF/SHARED_REF: target type name
};

struct OwnedActionDesc {
    std::string name;
    FjellActionType type;
    FjellActionTrigger trigger{FJELL_TRIGGER_TRIGGERED};
    void* trampoline{nullptr};
};

struct OwnedScriptType {
    std::string name;
    uint32_t type_id;
    ScriptComponent* (*create)();
    std::vector<OwnedFieldDesc> fields;
    std::vector<OwnedActionDesc> actions;
    void (*trace)(void* instance, void* visitor){nullptr};
    bool loaded{true};
};

struct OwnedUiWidgetType {
    std::string name;
    UiWidget* (*create)();
    std::vector<OwnedFieldDesc> fields;
    void (*trace)(void* instance, void* visitor){nullptr};
    bool loaded{true};
};

// Backward compat alias
using OwnedUiComponentType = OwnedUiWidgetType;

class ScriptManager {
public:
    ScriptManager();
    ~ScriptManager();

    ScriptManager(const ScriptManager&) = delete;
    ScriptManager& operator=(const ScriptManager&) = delete;
    ScriptManager(ScriptManager&&) = delete;
    ScriptManager& operator=(ScriptManager&&) = delete;

    Result<> load(const std::filesystem::path& so_path);
    void unload(Scene& scene);
    Result<> reload(Scene& scene, const std::filesystem::path& so_path);

    void prepare(Scene& scene, const Input& input, const Clock& clock,
                 PhysicsWorld* physics = nullptr);
    void update(Scene& scene, const Input& input, const Clock& clock,
                PhysicsWorld* physics = nullptr);

    Result<> add_script(Node3D& obj, const std::string& type_name);
    void remove_script(Node3D& obj, const std::string& type_name);
    void draw_inspector(Node3D& obj);

    void serialize_all(Scene& scene);
    void deserialize_all(Scene& scene);

    void call_begin_play(Scene& scene);
    void call_end_play(Scene& scene);

    void trace_doomed_refs(Scene& scene, std::span<const uint32_t> doomed_ids);
    void dispatch_collision_events(Scene& scene, class PhysicsWorld& physics);

    [[nodiscard]] const OwnedScriptType* find_script_type(const std::string& name) const;
    [[nodiscard]] const std::vector<OwnedScriptType>& script_types() const { return script_type_list_; }

    [[nodiscard]] const OwnedUiWidgetType* find_ui_widget_type(const std::string& name) const;
    [[nodiscard]] const OwnedUiWidgetType* find_ui_type_by_tag(const std::string& tag) const; // backward compat
    [[nodiscard]] const std::vector<OwnedUiWidgetType>& ui_types() const { return ui_type_list_; }

    [[nodiscard]] bool is_loaded() const { return library_.is_loaded(); }
    [[nodiscard]] static uint32_t hash_type_name(const std::string& name);

    // Set engine services for asset ref inspector and script API
    static void set_input(class Input* input);
    static void set_resource_manager(class ResourceManager* rm);
    static void set_active_project(const class Project* project);
    static void set_vfx_subsystem(class VfxSubsystem* vfx);
    static void set_audio_subsystem(class AudioSubsystem* audio);
    static void set_camera_subsystem(class CameraSubsystem* camera);
    static void set_vulkan_context(class VulkanContext* vk);

#ifndef NDEBUG
    Result<> poll_reload(Scene& scene);
    void watch(const std::filesystem::path& so_path);
#endif

private:
    void populate_world(const Input& input, const Clock& clock,
                        PhysicsWorld* physics);
    void build_registry();

    void auto_serialize_fields(const std::vector<OwnedFieldDesc>& fields,
                               const void* base, nlohmann::json& out) const;
    void auto_deserialize_fields(const std::vector<OwnedFieldDesc>& fields,
                                 void* base, const nlohmann::json& in) const;

    platform::SharedLibrary library_;
    std::filesystem::path so_path_;
    std::filesystem::path loaded_temp_path_;

    std::unordered_map<uint32_t, std::size_t> script_type_map_;
    std::vector<OwnedScriptType> script_type_list_;
    std::vector<OwnedUiComponentType> ui_type_list_;

    World world_{};

#ifndef NDEBUG
    platform::FileWatcher watcher_;
    bool reload_pending_{false};
    std::chrono::steady_clock::time_point last_reload_time_{};
#endif
};

} // namespace fjell
