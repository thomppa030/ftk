#pragma once

#include "core/delegate.hpp"

#include <cstdint>
#include <memory>

namespace fjell {

class AudioSubsystem;
class CameraSubsystem;
class Clock;
class GameUi;
class Input;
class InputActionMap;
class PhysicsSubsystem;
class PlaySession;
class ProjectManager;
class ResourceManager;
class Scene;
class ScreenDebugQueue;
class ScriptSubsystem;
class SpriteAnimSubsystem;
class UiSubsystem;
class VfxSubsystem;
class VulkanContext;
class Window;

enum class PlayState : uint8_t { stopped, playing };

/// Everything a play session needs to start, run and stop. Engine owns all
/// of these and fills the struct once; the controller only borrows them.
struct PlayServices {
    Scene* scene{nullptr};
    Input* input{nullptr};
    Clock* clock{nullptr};
    InputActionMap* input_map{nullptr};
    ProjectManager* project_mgr{nullptr};
    ResourceManager* resources{nullptr};
    VulkanContext* vulkan{nullptr};
    Window* window{nullptr};
    GameUi* game_ui{nullptr};
    ScreenDebugQueue* screen_debug{nullptr};
    ScriptSubsystem* scripts{nullptr};
    PhysicsSubsystem* physics{nullptr};
    VfxSubsystem* vfx{nullptr};
    CameraSubsystem* camera{nullptr};
    AudioSubsystem* audio{nullptr};
    SpriteAnimSubsystem* sprite_anim{nullptr};
    UiSubsystem* ui{nullptr};
};

/// Owns the play-mode lifecycle: the state machine, the subsystem start and
/// stop order, and the session that holds the pre-play snapshot. The editor
/// presents the state and reacts to transitions through the delegates; it
/// never drives the transitions itself beyond calling start()/stop().
///
/// In game mode (a shipped build) there is no snapshot and stop() closes the
/// window instead of restoring the editor scene.
class PlayController {
public:
    PlayController();
    ~PlayController();

    PlayController(const PlayController&) = delete;
    PlayController& operator=(const PlayController&) = delete;
    PlayController(PlayController&&) = delete;
    PlayController& operator=(PlayController&&) = delete;

    void init(PlayServices services, bool game_mode);

    /// No-op unless stopped.
    void start();
    /// No-op unless playing. In game mode this requests window close.
    void stop();
    void toggle();

    [[nodiscard]] PlayState state() const { return state_; }
    [[nodiscard]] bool playing() const { return state_ != PlayState::stopped; }

    /// No-op unless playing. Backed by Clock, so a script calling
    /// fjell_time_set_paused shows up here too.
    void pause();
    void resume();
    void toggle_pause();
    [[nodiscard]] bool paused() const;

    /// Resume for exactly one tick, then re-pause. No-op unless playing.
    void step();

    void set_time_scale(float scale);
    [[nodiscard]] float time_scale() const;

    /// Called once per frame from Engine::run, before the play phases tick.
    /// Turns a pending step() into exactly one unpaused frame.
    void begin_frame();

    /// Load every InputComponent's action set and push its mapping contexts.
    /// Called at play start and again whenever a node with an InputComponent
    /// is spawned during play, so its bindings can dispatch.
    void load_input_contexts();

    /// Fires after every subsystem has started and the scene is live.
    Delegate<void()> on_started;
    /// Fires before subsystems stop, while the play scene is still intact.
    Delegate<void()> on_stopping;
    /// Fires after the editor scene has been restored.
    Delegate<void()> on_stopped;
    Delegate<void()> on_paused;
    Delegate<void()> on_resumed;

private:
    void check_component_serialization() const;

    PlayServices s_;
    bool game_mode_{false};
    PlayState state_{PlayState::stopped};
    std::unique_ptr<PlaySession> session_;

    // Set by step(), consumed by begin_frame(): resume this frame, then
    // re-pause at the start of the next one.
    bool stepping_{false};
};

} // namespace fjell
