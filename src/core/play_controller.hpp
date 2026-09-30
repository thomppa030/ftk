#pragma once

#include "core/delegate.hpp"
#include "core/fixed_step_stats.hpp"
#include "core/fixed_timestep.hpp"
#include "core/step_input_latch.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>

namespace fjell {

class AudioSubsystem;
class CameraSubsystem;
class Clock;
class EnvironmentSubsystem;
class Input;
class InputActionMap;
class OffscreenPass;
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
enum class PlayPresentation : uint8_t { docked, fullscreen };

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
    ScreenDebugQueue* screen_debug{nullptr};
    ScriptSubsystem* scripts{nullptr};
    PhysicsSubsystem* physics{nullptr};
    VfxSubsystem* vfx{nullptr};
    CameraSubsystem* camera{nullptr};
    AudioSubsystem* audio{nullptr};
    SpriteAnimSubsystem* sprite_anim{nullptr};
    EnvironmentSubsystem* environment{nullptr};
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

    /// Materializes nodes spawned since the last flush and gives them their
    /// play-time state: input contexts, physics bodies, and their scripts'
    /// instantiate + begin_play. Loops until the queue is empty, since a
    /// spawned node's begin_play may spawn more. Runs after each tick phase
    /// and once during start(), between the script and UI subsystems, so a
    /// widget starting up can find what begin_play just spawned. Only
    /// meaningful while a session is starting or playing.
    void flush_spawned();
    void toggle();

    /// Destroys the HUD target now, while ImGui and Vulkan are still alive.
    /// Call once, from Engine's destructor, before it starts tearing down
    /// ImGui — the default destructor would otherwise run this same
    /// teardown later, after ImGui is already gone, since hud_target_ is a
    /// plain member that destructs on its own. A no-op if the target was
    /// never created.
    void shutdown();

    [[nodiscard]] PlayState state() const { return state_; }
    [[nodiscard]] bool playing() const { return state_ != PlayState::stopped; }

    /// No-op unless playing. Backed by Clock, so a script calling
    /// fjell_time_set_paused shows up here too.
    void pause();
    void resume();
    void toggle_pause();
    [[nodiscard]] bool paused() const;

    /// Resume for exactly one frame, which advances the simulation by exactly
    /// one fixed step, then re-pause. No-op unless playing.
    void step();

    /// The simulation's clock for the session: how many fixed steps each
    /// frame runs. Reset when play starts, at the project's fixed tick rate.
    [[nodiscard]] FixedTimestep& fixed_timestep() { return fixed_timestep_; }

    /// How the session's steps are going: filled in by the engine's step
    /// loop, read by the stats panel. Reset when play starts and stops.
    [[nodiscard]] FixedStepStats& step_stats() { return step_stats_; }
    [[nodiscard]] const FixedStepStats& step_stats() const { return step_stats_; }

    /// The button edges each fixed step sees, recorded frame by frame in
    /// begin_frame(). Reset when play starts.
    [[nodiscard]] StepInputLatch& step_input() { return step_input_; }

    void set_time_scale(float scale);
    [[nodiscard]] float time_scale() const;

    /// Docked (default) draws the scene inside the editor's viewport panel,
    /// with every other panel still live around it. Fullscreen takes over the
    /// whole window, the same view game mode and `--play` already use.
    /// Game mode ignores this — it is always fullscreen, there being no
    /// editor to dock into. Persisted per project (ProjectConfig::PlayConfig)
    /// so the choice survives a restart.
    [[nodiscard]] PlayPresentation presentation() const;
    void toggle_presentation();

    /// Whether the running game currently has input focus. Fullscreen is
    /// always focused — there's no editor chrome competing for input.
    /// Docked starts focused too, the moment play starts (see start()) —
    /// picking and the gizmo are fully disabled for as long as this is true
    /// (see SceneEditorContext::draw()), so there is nothing left in the
    /// editor for a click meant for the game to land on instead. Feeds
    /// Input::set_game_focus() once per frame from begin_frame(), so
    /// nothing else needs to check it directly.
    [[nodiscard]] bool input_focused() const;

    /// Called when the player clicks the docked viewport's image after
    /// releasing focus with Shift+Esc — see SceneEditorContext::draw_play_toolbar.
    /// No-op in fullscreen, which is always focused already.
    void capture_input();

    /// Called on Shift+Esc while docked play has focus — hands input back
    /// to the editor (re-enabling picking and the gizmo) without stopping
    /// play. A second Shift+Esc (now that nothing has focus to give up)
    /// falls through to stop(), matching the plan's "release, then stop"
    /// shortcut design.
    void release_input();

    /// F10 toggle. Ejected: the viewport shows and flies with its own
    /// camera, same as editing, while the game keeps simulating with its
    /// own camera regardless (UiSubsystem::set_camera, screen debug text —
    /// they describe the game, not whatever view happens to be on screen).
    /// Possessed (the default) shows the game's own CameraNode and the
    /// viewport camera does nothing useful, same as before eject existed.
    /// Reset to possessed on stop() so the next play session always starts
    /// there regardless of how the last one ended.
    [[nodiscard]] bool ejected() const { return ejected_; }

    /// Ejecting drops fullscreen to docked first if needed — flying free
    /// only makes sense with an actual viewport panel to show it in and
    /// editor chrome around it, neither of which fullscreen has — then
    /// releases input focus the same way Shift+Esc does, so WASD drives
    /// the fly camera instead of also still reaching the player through a
    /// game that never gave up focus. Re-possessing (F10 again) does not
    /// auto-recapture; click the viewport to hand focus back to the game.
    void toggle_eject();

    /// Called once per frame from Engine::run, before the play phases tick.
    /// Turns a pending step() into exactly one unpaused frame.
    void begin_frame();

    /// Load every InputComponent's action set and push its mapping contexts.
    /// Called at play start and again whenever a node with an InputComponent
    /// is spawned during play, so its bindings can dispatch.
    void load_input_contexts();

    /// Where the scene image occupied the screen last frame — the docked
    /// viewport panel's rect, or the fullscreen play view's window rect. Set
    /// by Engine right after each frame's draw_ui()/draw_play_overlay(), for
    /// apply_pending_hud_resize() to consume at the very start of the next
    /// frame (see below for why the size is one frame behind on purpose).
    void set_hud_rect(glm::vec2 img_pos, glm::vec2 img_size) {
        hud_img_pos_ = img_pos;
        hud_img_size_ = img_size;
    }

    /// Resizes the HUD target to match the last set_hud_rect() call, if it
    /// differs. Called once per frame from Engine::run, before
    /// draw_ui()/draw_play_overlay() run — so any resize (which destroys and
    /// recreates the target's ImGui descriptor) completes before this frame's
    /// ImGui draw list can read hud_texture(). Applying it any later leaves a
    /// one-frame window where a draw command already queued with the old
    /// descriptor gets submitted after that descriptor was destroyed —
    /// exactly the OffscreenPass::resize/cleanup_images ordering the UI
    /// editor's own preview avoids by never resizing a slot a frame in
    /// flight might still reference. The HUD's own size therefore always
    /// trails the panel's by one frame, the same way apply_resize() already
    /// resizes viewport offscreens "using last frame's panel size".
    void apply_pending_hud_resize();

    /// Composite every screen-space UiSubsystem instance into the (already
    /// correctly sized) HUD target. No-op unless playing. The result is drawn
    /// with ImGui::Image over the scene image by whichever presentation is
    /// active, at the same rect — so the HUD is a texture the game rendered,
    /// not the swapchain. Registered as VulkanContext's play_hud_callback_;
    /// not called directly.
    void record_hud(uint32_t frame_index);

    /// The HUD target's ImGui descriptor, valid only after record_hud() has
    /// run at least once this session. Null while stopped or before the
    /// first size is known.
    [[nodiscard]] VkDescriptorSet hud_texture();

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

    FixedTimestep fixed_timestep_;
    FixedStepStats step_stats_;
    StepInputLatch step_input_;

    // Docked-only: true unless the player has released focus with Shift+Esc
    // and not yet clicked the viewport to take it back. Starts true on
    // start() — see the comment on input_focused(). Meaningless in
    // fullscreen — input_focused() short-circuits to true there regardless.
    bool captured_{true};

    // F10 camera eject — see ejected(). Reset to false in stop().
    bool ejected_{false};

    // Set when F10 ejects from fullscreen, so presentation() reports docked
    // for the rest of this session without touching the project's saved
    // preference — ejecting is a transient view choice, not a change to
    // what play mode defaults to next time. Cleared on stop() and on
    // toggle_presentation() (an explicit F11 wins over it either way).
    bool docked_override_{false};

    // Screen-space HUD, composited over the scene image by whichever
    // presentation is active. Created lazily by apply_pending_hud_resize()
    // once playing (its extent depends on the viewport panel size, which
    // isn't known yet at start()).
    std::unique_ptr<OffscreenPass> hud_target_;
    glm::vec2 hud_img_pos_{0.0f};
    glm::vec2 hud_img_size_{0.0f};

    // stop() cannot destroy hud_target_ synchronously: it can run mid-frame
    // (the toolbar's Stop button, mid-draw) after this same frame's
    // ImGui::Image(hud_texture()) already queued a draw referencing it, and
    // before that draw is actually submitted (in overlay_callback_, later in
    // record_command_buffer). stop() sets this flag instead;
    // apply_pending_hud_resize() destroys the target at the one point already
    // established as safe — the very start of the next frame, before that
    // frame can read hud_texture() at all.
    bool hud_pending_destroy_{false};
};

} // namespace fjell
