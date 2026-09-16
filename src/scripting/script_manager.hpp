#pragma once

#include "core/result.hpp"
#include "scripting/fjell/api.h"
#include "scripting/fjell/registry.hpp"
#include "scripting/fjell/world.hpp"
#include "scripting/timer_manager.hpp"
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
    std::string tooltip;        // hover help (empty if none)
    std::string category;       // group header (empty if none)
    bool readonly{false};
    bool replicated{false};     // [[fj::replicated]] — synced over the network
};

struct OwnedScriptType {
    std::string name;
    uint32_t type_id;
    ScriptComponent* (*create)();
    std::vector<OwnedFieldDesc> fields;
    void (*trace)(void* instance, void* visitor){nullptr};
    const FjellRpcDesc* rpcs{nullptr};  // borrowed from the game .so's static table
    uint32_t rpc_count{0};
    bool loaded{true};
};

struct OwnedUiActionDesc {
    std::string name;
    uint8_t param_type{0}; // 0=none, 1=int, 2=float, 3=vec2
};

struct OwnedUiWidgetType {
    std::string name;
    UiWidget* (*create)();
    std::vector<OwnedFieldDesc> fields;
    std::vector<OwnedUiActionDesc> actions;
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

    // Instantiate scripts and call begin_play for nodes spawned mid-play (the
    // play-start deserialize/begin_play passes already ran, so freshly-spawned
    // nodes would otherwise have no live script instances). ids are the nodes
    // returned by Scene::flush_spawned().
    void initialize_spawned(Scene& scene, const std::vector<uint32_t>& ids);

    void trace_doomed_refs(Scene& scene, std::span<const uint32_t> doomed_ids);
    void dispatch_collision_events(Scene& scene, class PhysicsWorld& physics);

    // Network connection events: call every script component's on_player_*
    // virtual. Fired on the server when a peer connects/disconnects. peer is the
    // connecting/leaving PeerId (uint32_t in the SDK).
    /// Tell every script the weather or the part of the day changed. The
    /// phases are the SDK's WeatherPhase and TimePhase as ints, so this
    /// header does not pull the SDK in.
    void dispatch_weather_changed(Scene& scene, uint32_t event, int phase);
    void dispatch_time_phase_changed(Scene& scene, int phase);

    void dispatch_player_connected(Scene& scene, uint32_t peer);
    void dispatch_player_disconnected(Scene& scene, uint32_t peer);

    [[nodiscard]] const OwnedScriptType* find_script_type(const std::string& name) const;
    [[nodiscard]] const std::vector<OwnedScriptType>& script_types() const { return script_type_list_; }

    [[nodiscard]] const OwnedUiWidgetType* find_ui_widget_type(const std::string& name) const;
    [[nodiscard]] const OwnedUiWidgetType* find_ui_type_by_tag(const std::string& tag) const; // backward compat
    [[nodiscard]] const std::vector<OwnedUiWidgetType>& ui_types() const { return ui_type_list_; }

    [[nodiscard]] bool is_loaded() const { return library_.is_loaded(); }
    [[nodiscard]] static uint32_t hash_type_name(const std::string& name);

    [[nodiscard]] TimerManager& timers() { return timers_; }

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
    TimerManager timers_;

#ifndef NDEBUG
    platform::FileWatcher watcher_;
    bool reload_pending_{false};
    std::chrono::steady_clock::time_point last_reload_time_{};
#endif
};

} // namespace fjell
