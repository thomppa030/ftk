#pragma once

#include "core/delegate.hpp"
#include "core/result.hpp"
#include "scripting/fjell/api.h"
#include "scripting/fjell/registry.hpp"
#include "scripting/fjell/world.hpp"
#include "scripting/timer_manager.hpp"
#include "platform/shared_library.hpp"
#include "renderer/resources/fjsl_parser.hpp"
#include "scripting/reflected_types.hpp"
#include "scene/component_type.hpp"
#include "scene/script_host.hpp"

#ifndef NDEBUG
#include "platform/file_watcher.hpp"
#endif

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fjell {

class ScriptComponent;
class UiWidget;
using UiComponent = UiWidget; // backward compat
class Scene;
class Input;
class Clock;
class PhysicsWorld;
struct Node;
struct Node3D;
struct ScriptEntry;
struct FixedStep;

struct OwnedScriptType {
    std::string name;
    uint32_t type_id;
    // How an instance is built in memory the engine provides: its type's slot
    // in the scene's pool.
    uint32_t size{0};
    uint32_t align{0};
    ScriptComponent* (*construct)(void* memory){nullptr};
    // What the pools of this type point at. The list holding this entry is
    // rebuilt only between an unload, which drops those pools, and the next
    // load, so the address holds for as long as any instance lives.
    ComponentType component{};
    // [[fj::tick(order = n)]]: where the type's scripts run among the others,
    // lower earlier; types of equal order go by name.
    int32_t tick_order{0};
    // FjellPolledHook bits: which of tick, fixed_tick and late_tick the type
    // overrides. It is walked only for those.
    uint32_t polls{0};
    std::vector<OwnedFieldDesc> fields;
    void (*trace)(void* instance, void* visitor){nullptr};
    const FjellRpcDesc* rpcs{nullptr};  // borrowed from the game .so's static table
    uint32_t rpc_count{0};
    bool loaded{true};
    // Instances of this type may be treated as a FunctionalTest. The C ABI
    // carries no base-class information, so the game module reports it at
    // registration and this is the only thing that makes the cast sound.
    bool is_functional_test{false};
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

class ScriptManager final : public ScriptHost {
public:
    ScriptManager();
    ~ScriptManager();

    ScriptManager(const ScriptManager&) = delete;
    ScriptManager& operator=(const ScriptManager&) = delete;
    ScriptManager(ScriptManager&&) = delete;
    ScriptManager& operator=(ScriptManager&&) = delete;

    Result<> load(const std::filesystem::path& so_path);
    void unload(Scene& scene);
    /// Swap in a rebuilt game library. While the game is running, the scripts
    /// it replaces get their end_play first and the new ones their
    /// begin_play after, so a reload is the same whichever path asked for it.
    Result<> reload(Scene& scene, const std::filesystem::path& so_path);

    void prepare(Scene& scene, const Input& input, const Clock& clock,
                 PhysicsWorld* physics = nullptr);
    void update(Scene& scene, const Input& input, const Clock& clock,
                PhysicsWorld* physics = nullptr);

    /// Run one fixed step's worth of scripting: every script's fixed_tick(),
    /// then the timers, advanced by the step. Timers are gameplay, and
    /// stepping them with the simulation is what lets them replay.
    void fixed_update(Scene& scene, const FixedStep& step);

    /// Run every script's late_tick(), once the frame's steps have run and
    /// what they moved is blended where it will be drawn.
    void late_update(Scene& scene, float dt);

    /// While on, a script whose tick() or late_tick() writes simulation state
    /// that the simulation then throws away, or makes depend on the frame
    /// rate, is reported, once per script and node: a dynamic body's position
    /// or rotation (physics puts it back), an Intent character's horizontal
    /// velocity (the engine sets it from move_intent) or a Velocity
    /// character's velocity (it belongs in fixed_tick). Editor builds turn it
    /// on; a shipped game does not look.
    void set_write_reports(bool on) { write_reports_on_ = on; }

    /// The reports raised since the last call, for the test runner, which
    /// fails the running test with them.
    [[nodiscard]] std::vector<std::string> take_write_reports();

    /// Puts a script of `type_name` on the node at `index` (the end when
    /// past it), with `fields` as its values. It is made live when the library
    /// has the type and the node is in a scene; a type the library lacks is
    /// kept as its fields, as a scene file keeps it. While the game is running
    /// it starts at once: its begin_play runs before this returns.
    /// @return The live script, or null when there is none: the type isn't
    ///         loaded, the node is in no scene, or its begin_play took it off
    ///         the node again.
    ScriptComponent* add_script(Node3D& obj, const std::string& type_name,
                                const nlohmann::json& fields = {},
                                std::size_t index = SIZE_MAX);
    /// Takes the node's script of `type_name` off it. One that started in a
    /// running game gets its end_play first and lets go of what it registered
    /// with the engine (see release()). Nothing when the node has none.
    void remove_script(Node& node, const std::string& type_name);

    /// Makes `entry`'s live instance from its saved fields, replacing any
    /// it had. An entry whose type isn't in the loaded library keeps its
    /// saved fields and gets no instance.
    void instantiate(Node3D& obj, ScriptEntry& entry);
    /// The script's field values as the scene file stores them: read from
    /// the instance, or the saved fields when it has none.
    [[nodiscard]] nlohmann::json fields_of(const ScriptEntry& entry) const;
    /// Sets the script's fields to `fields` (as fields_of() gives them), on
    /// the instance when it has one and in its saved fields.
    void set_fields(ScriptEntry& entry, const nlohmann::json& fields);

    void serialize_all(Scene& scene);
    void deserialize_all(Scene& scene);

    void call_begin_play(Scene& scene);
    void call_end_play(Scene& scene);

    /// Whether the game is running. While it is, a script added to a node
    /// starts at once, and a reload ends the scripts it replaces and starts
    /// the new ones.
    void set_playing(bool playing) {
        playing_ = playing;
        // Each session reports afresh.
        if (playing) {
            reported_writes_.clear();
            blamed_writes_.clear();
        }
    }
    [[nodiscard]] bool playing() const { return playing_; }

    /// End the scripts on nodes about to be destroyed while play goes on:
    /// each gets its end_play(), then lets go of everything it registered
    /// with the engine (see release()), and is destroyed. Safe to call again
    /// for the same nodes; a script already ended is gone.
    void end_play_destroyed(Scene& scene, std::span<const uint32_t> doomed_ids);

    /// Let go of the input bindings and timers `owner` registered by its own
    /// pointer, so nothing calls into it once it is gone. Scripts register
    /// with `this` as their own class, which is the address of the whole
    /// object; both it and the base's address are let go, so a script that
    /// derives from something else first is covered too.
    template <typename Base>
    void release(const Base* owner) {
        release_address(owner);
        release_address(dynamic_cast<const void*>(owner));
    }

    /// Create the scripts on nodes that joined the game mid-play and run their
    /// begin_play (the play-start passes already ran without them). What
    /// those begin_plays spawn is left pending, for the caller's next round:
    /// see PlayController::flush_spawned().
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

    /// A node leaving its scene: its live scripts are ended if they had
    /// started in a running game, saved into their entries, and let go.
    void scripts_leaving(Node& node) override;
    /// A node put back into its scene: its scripts are made again from their
    /// entries, and started if the game is running.
    void scripts_returned(Node& node) override;

    [[nodiscard]] const OwnedScriptType* find_script_type(const std::string& name) const;
    /// The loaded script type whose pool is `id`, or null.
    [[nodiscard]] const OwnedScriptType* find_script_type_by_id(ComponentTypeId id) const;
    [[nodiscard]] const std::vector<OwnedScriptType>& script_types() const { return script_type_list_; }

    [[nodiscard]] const OwnedUiWidgetType* find_ui_widget_type(const std::string& name) const;
    [[nodiscard]] const OwnedUiWidgetType* find_ui_type_by_tag(const std::string& tag) const; // backward compat
    [[nodiscard]] const std::vector<OwnedUiWidgetType>& ui_types() const { return ui_type_list_; }
    /// The module's reflected structs (widget Models and what they hold) and
    /// pipes. Valid until the next load.
    [[nodiscard]] const ReflectedTypes& reflected_types() const { return reflected_types_; }

    [[nodiscard]] bool is_loaded() const { return library_.is_loaded(); }
    /// Moves each time a game library loads, a reload included: what the
    /// game declares may have changed.
    [[nodiscard]] uint32_t library_revision() const { return library_revision_; }

    /// Raised just before a game library is unloaded, a reload included:
    /// whatever holds objects the library built, or calls into its code,
    /// lets go of them here, while the library is still there.
    Delegate<void()> on_unloading;
    [[nodiscard]] static uint32_t hash_type_name(const std::string& name);

    [[nodiscard]] TimerManager& timers() { return timers_; }

#ifndef NDEBUG
    Result<> poll_reload(Scene& scene);
    void watch(const std::filesystem::path& so_path);
#endif

private:
    /// What a script outside the step could write that the simulation owns.
    struct SimulatedState {
        glm::vec3 position{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 velocity{0.0f};
    };
    /// A node whose state the simulation owns: a dynamic body or a simulated
    /// character.
    [[nodiscard]] static bool simulation_owns(const Node& node);
    [[nodiscard]] static SimulatedState simulated_state(const Node& node);
    /// Run a hook on every live script, type by type in tick order and, within
    /// a type, in the order its scripts joined the scene.
    template <typename Hook>
    void run_scripts(Scene& scene, const char* hook_name, Hook&& hook);
    /// The same for a polled hook (tick, fixed_tick, late_tick; `hook_bit` is
    /// its FjellPolledHook bit), called as hook(script, type): only on types
    /// that override it. After a frame hook (tick, late_tick), a script on a
    /// node whose state the simulation owns is checked for having written that
    /// state.
    template <typename Hook>
    void run_polled(Scene& scene, uint32_t hook_bit, const char* hook_name, Hook&& hook);
    /// Report what the script of type `type_name` wrote on `node` that the
    /// simulation owns, judged against the state before it ran.
    void report_outside_writes(const Node& node, const std::string& type_name,
                               const char* hook_name, const SimulatedState& before);
    void report_write(const std::string& key, std::string message);
    /// The state the simulation owns of every node that has any, taken before
    /// a frame pass (empty while reports are off). After it,
    /// report_foreign_writes() catches a script writing another node's, which
    /// the check after each script, looking at its own node, cannot see.
    [[nodiscard]] std::vector<std::pair<uint32_t, SimulatedState>> owned_states(Scene& scene) const;
    void report_foreign_writes(Scene& scene,
                               std::span<const std::pair<uint32_t, SimulatedState>> before,
                               const char* hook_name);

    void release_address(const void* owner);
    /// Run the script's begin_play, unless it has already run.
    void start_script(Node& node, std::size_t index);

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
    ReflectedTypes reflected_types_; // what script_type_list_'s struct fields point into
    std::vector<OwnedScriptType> script_type_list_;
    // The loaded script types in the order their scripts run: by tick order,
    // then by type name, which is reproducible where registration order (the
    // link order) is not.
    std::vector<const OwnedScriptType*> tick_order_;
    // Each script type name's pool id, kept across reloads so a type keeps its
    // id for the whole session.
    std::unordered_map<std::string, ComponentTypeId> pool_ids_;
    std::vector<OwnedUiComponentType> ui_type_list_;

    World world_{};
    TimerManager timers_;
    bool playing_{false};

    bool write_reports_on_{false};
    uint32_t library_revision_{0};
    std::vector<std::string> write_reports_;
    // Script, node and kind of every write already reported this session,
    // and the node and kind of every one a node's own script was named for.
    std::unordered_set<std::string> reported_writes_;
    std::unordered_set<std::string> blamed_writes_;

#ifndef NDEBUG
    platform::FileWatcher watcher_;
    bool reload_pending_{false};
    std::chrono::steady_clock::time_point last_reload_time_{};
#endif
};

} // namespace fjell
