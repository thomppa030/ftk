// component.hpp — ScriptComponent base class for Fjell game code
#pragma once

#include "fjell/environment_api.hpp"
#include "fjell/fixed_step.hpp"
#include "fjell/world.hpp"
#include "fjell/transform.hpp"

#include <cstdint>

namespace fjell {

struct Node3D;  // the node as scripts see it — defined in fjell/node.hpp
class ScriptManager;

// Network peer identity. Identical alias to fjell/net_api.hpp's PeerId, so both
// headers can be included together. Declared here to avoid pulling the net API
// (and its NetRole enum, which collides with the engine-internal one) into every
// component TU.
using PeerId = uint32_t;

/// Base class for gameplay scripts. Derive from this, add `FJELL_COMPONENT(T)`
/// inside the class, and the component becomes attachable to any node in the
/// editor and spawnable from `Node3D::add_component<T>()`.
///
/// Fields marked `[[fj::property]]` show up in the inspector and are saved with
/// the scene. Methods marked `[[fj::action]]` can be bound to input actions,
/// and `[[fj::rpc(...)]]` methods are callable across the network.
///
/// @code
/// #include "fjell/component.hpp"
/// #include "fjell/registry.hpp"
///
/// class Spinner : public fjell::ScriptComponent {
///     FJELL_COMPONENT(Spinner)
/// public:
///     [[fj::property]] [[fj::range(0, 720)]] float degrees_per_second = 90.0f;
///
///     void tick(float dt) override {
///         float radians = glm::radians(degrees_per_second * dt);
///         transform().rotation = glm::rotate(transform().rotation, radians,
///                                            glm::vec3{0, 1, 0});
///     }
/// };
/// @endcode
class ScriptComponent {
public:
    virtual ~ScriptComponent() = default;

    /// Called once when play starts, or immediately on spawn if the game is
    /// already running. All engine subsystems and sibling components exist by
    /// now — this is the place to resolve nodes, bind input, and start timers.
    /// Prefer it over the constructor, which runs before the component is
    /// attached to a node (`transform()` and `self()` are not usable there).
    virtual void begin_play() {}

    /// Called every frame while play is running, before the simulation
    /// steps: react to input and set what things should do here, so this
    /// frame's steps already see it. Move things by physics in `fixed_tick()`,
    /// and follow things on screen in `late_tick()`.
    /// @param dt Seconds since the previous frame, already scaled by
    ///           `Time::set_scale()`. Zero while `Time::pause()` is active.
    ///           For a type with `[[fj::tick(interval = s)]]`, called about
    ///           every `s` seconds instead, with the game time since its last
    ///           tick.
    virtual void tick(float dt) { (void)dt; }

    /// Called once per fixed step of the simulation while play is running,
    /// before physics moves anything: none, one or several times a frame,
    /// always with the same step length. Custom movement, forces, abilities
    /// and anything that has to replay exactly belong here. Timers fire in
    /// these steps too.
    /// @param step The step's length, time and index, and `step.input`, the
    ///             input this step sees. `delta_time()` reads the step's
    ///             length here.
    virtual void fixed_tick(const FixedStep& step) { (void)step; }

    /// Called every frame after the simulation has stepped, when every node
    /// sits where it will be drawn. Cameras and anything that follows what is
    /// on screen belong here: read other nodes through `render_transform()`,
    /// not `position()`, or the follower runs up to a step ahead of what it
    /// follows and judders.
    /// @param dt Seconds since the previous frame, as `tick()` receives.
    virtual void late_tick(float dt) { (void)dt; }

    /// Called when play stops, the node is destroyed, or scripts hot-reload.
    /// Release anything that outlives the component here. Its own input
    /// bindings and timers go with it without help.
    virtual void end_play() {}

    /// Called when this node's collider begins touching a solid (non-trigger)
    /// collider. Requires a ColliderComponent on both nodes.
    /// @param other_id Node ID of the other collider — `Ref<Node3D>{other_id}` refers to it.
    /// @param point    World-space contact position.
    /// @param normal   World-space contact normal, pointing away from the other body.
    virtual void on_collision_enter(uint32_t other_id, const glm::vec3& point, const glm::vec3& normal) {
        (void)other_id; (void)point; (void)normal;
    }

    /// Called when a solid contact ends, including when the other node is
    /// destroyed — do not assume `other_id` still resolves to a live node.
    /// @param other_id Node ID of the collider that was being touched.
    virtual void on_collision_exit(uint32_t other_id) { (void)other_id; }

    /// Called when something enters a collider marked `is_trigger`. Triggers
    /// report overlaps without generating a physical response.
    /// @param other_id Node ID of the node that entered.
    virtual void on_trigger_enter(uint32_t other_id) { (void)other_id; }

    /// Called when something leaves a trigger collider, including when the
    /// other node is destroyed.
    /// @param other_id Node ID of the node that left.
    virtual void on_trigger_exit(uint32_t other_id) { (void)other_id; }

    /// Fires on the SERVER only, when a client finishes connecting. Spawn and
    /// assign that peer's player here via `net::spawn(preset, pos, peer)`.
    /// @param peer The connecting peer's ID — pass as the owner when spawning.
    /// The weather changed: a new event started, or the one playing moved
    /// into its next phase. `event` is the event's handle id, matching a
    /// `Asset<Weather>`'s; `phase` is where it now is.
    /// @code
    /// void on_weather_changed(uint32_t event, fjell::WeatherPhase phase) override {
    ///     if (phase == fjell::WeatherPhase::Onset) close_the_shutters();
    /// }
    /// @endcode
    virtual void on_weather_changed(uint32_t event, WeatherPhase phase) {
        (void)event;
        (void)phase;
    }

    /// The day moved into another part: dawn, day, dusk or night.
    virtual void on_time_phase_changed(TimePhase phase) { (void)phase; }

    virtual void on_player_connected(PeerId peer) { (void)peer; }

    /// Fires on the SERVER only, when a client disconnects. Despawn anything
    /// owned by that peer here.
    /// @param peer The departing peer's ID.
    virtual void on_player_disconnected(PeerId peer) { (void)peer; }

    /// Node ID of the node this component is attached to, for the engine
    /// calls that still take ids. Stable for the node's lifetime; 0 is never
    /// a valid node.
    [[nodiscard]] uint32_t id() const { return id_; }

    /// Name of the owning node, as shown in the editor hierarchy.
    [[nodiscard]] const char* name() const { return name_; }

    /// This node's local position, rotation, and scale — relative to its
    /// parent, not the world. Writes take effect the same frame; no
    /// dirty-marking is needed.
    [[nodiscard]] Transform& transform() { return *transform_; }
    [[nodiscard]] const Transform& transform() const { return *transform_; }

    /// The node this component is on. It is there for as long as the script
    /// is, so there is nothing to check; pass it where a `Ref<Node3D>` is
    /// taken as it is.
    /// @code
    /// if (auto body = self().get_component<fjell::RigidbodyComponent>()) {
    ///     body->mass = 5.0f;
    /// }
    /// @endcode
    inline Node3D& self() const;  // defined in fjell/node.hpp

    // --- World state ---

    /// Raw keyboard and mouse state for this frame. Good for prototyping;
    /// prefer `[[fj::action]]` methods and `input` for shipping input, so
    /// bindings stay remappable.
    [[nodiscard]] const Input& input() const { return world_->input(); }

    /// Seconds since the previous frame — the value `tick()` gets unless its
    /// type ticks at an interval. Inside `fixed_tick()` it is the step's
    /// length. Useful from handlers that don't receive it as an argument.
    [[nodiscard]] float delta_time() const { return world_->delta_time; }

    /// Seconds since play started, scaled by `Time::set_scale()` and frozen
    /// while paused. Inside `fixed_tick()` it is the step's time.
    [[nodiscard]] float elapsed_time() const { return world_->elapsed_time; }

    /// Shared per-frame runtime state (input, timing, the fixed step's time).
    [[nodiscard]] World* world() { return world_; }
    [[nodiscard]] const World* world() const { return world_; }

    // --- Ticking ---

    /// Stop or resume the hooks the engine calls every frame or step:
    /// `tick()`, `fixed_tick()` and `late_tick()`. Everything that arrives as
    /// an event still reaches a script that does not tick — delegates, input
    /// actions, timers, collision and trigger callbacks, weather, time and
    /// peer events, RPCs and UI actions — so a script can sleep until
    /// something happens to it. Time spent not ticking does not count towards
    /// the dt of a type that ticks at an interval.
    /// @code
    /// void Door::begin_play() { set_tick_enabled(false); }
    /// void Door::on_trigger_enter(uint32_t) { set_tick_enabled(true); }  // tick while it opens
    /// @endcode
    void set_tick_enabled(bool enabled) { tick_enabled_ = enabled; }
    [[nodiscard]] bool tick_enabled() const { return tick_enabled_; }

private:
    friend class ScriptManager;
    Node3D* node_{nullptr};
    World* world_{nullptr};
    Transform* transform_{nullptr};
    uint32_t id_{0};
    const char* name_{""};
    bool tick_enabled_{true};
    // For a type that ticks at an interval: game time since its last tick(),
    // and until the next one is due.
    float tick_since_last_{0.0f};
    float tick_due_in_{0.0f};
};

using Component = ScriptComponent;

} // namespace fjell
