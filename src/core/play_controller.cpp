extern "C" void fjell_input_clear_connections();

#include "core/play_controller.hpp"

#include "core/clock.hpp"
#include "core/frame_context.hpp"
#include "core/input.hpp"
#include "core/input_action_map.hpp"
#include "core/input_component.hpp"
#include "core/log.hpp"
#include "core/play_session.hpp"
#include "core/project_manager.hpp"
#include "renderer/gpu/window.hpp"
#include "audio/audio_subsystem.hpp"
#include "environment/environment_subsystem.hpp"
#include "physics/physics_subsystem.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/systems/sprite_anim_subsystem.hpp"
#include "renderer/systems/vfx_subsystem.hpp"
#include "renderer/vulkan_context.hpp"
#include "scene/camera_subsystem.hpp"
#include "scene/component_registry.hpp"
#include "scene/scene.hpp"
#include "scripting/script_subsystem.hpp"
#include "ui/game_ui/game_ui.hpp"
#include "ui/game_ui/ui_subsystem.hpp"
#include "ui/game_ui/ui_widget_component.hpp"


#include <filesystem>

namespace fjell {

PlayController::PlayController() = default;
PlayController::~PlayController() = default;

void PlayController::init(PlayServices services, bool game_mode) {
    s_ = services;
    game_mode_ = game_mode;
    check_component_serialization();

    // Registered once here rather than called directly from Engine::run:
    // the HUD's barriers and dynamic-rendering calls must land inside the
    // main command buffer's recording scope, which is only open partway
    // through VulkanContext::record_command_buffer — the same reason the UI
    // editor's own preview uses ui_preview_callback_ instead of a direct call.
    s_.vulkan->set_play_hud_callback([this](uint32_t frame_index) {
        record_hud(frame_index);
    });
}

void PlayController::check_component_serialization() const {
    // The snapshot is the scene serializer, so a component that registers
    // without both halves would vanish on every play round trip. Reported
    // once at startup rather than discovered as a missing component later.
    ComponentRegistry::instance().for_each([](ComponentTypeId, const ComponentMeta& meta) {
        if (!meta.serialize || !meta.deserialize) {
            FJELL_CORE_ERROR("Component '{}' has no {} — it will not survive play mode",
                             meta.display_name,
                             !meta.serialize ? "serializer" : "deserializer");
        }
    });
}

void PlayController::toggle() {
    if (playing()) {
        stop();
    } else {
        start();
    }
}

void PlayController::pause() {
    if (!playing() || paused()) return;
    s_.clock->set_paused(true);
    s_.audio->on_paused();
    on_paused.broadcast();
}

void PlayController::resume() {
    if (!playing() || !paused()) return;
    s_.clock->set_paused(false);
    s_.audio->on_resumed();
    on_resumed.broadcast();
}

void PlayController::toggle_pause() {
    if (paused()) {
        resume();
    } else {
        pause();
    }
}

bool PlayController::paused() const {
    return playing() && s_.clock->is_paused();
}

void PlayController::step() {
    if (!playing()) return;
    stepping_ = true;
    resume();
}

void PlayController::set_time_scale(float scale) {
    s_.clock->set_time_scale(scale);
}

float PlayController::time_scale() const {
    return s_.clock->time_scale();
}

PlayPresentation PlayController::presentation() const {
    // Game mode has no editor to dock into — it's always fullscreen,
    // regardless of what the project happens to have saved.
    if (game_mode_) return PlayPresentation::fullscreen;
    if (docked_override_) return PlayPresentation::docked;
    auto* project = s_.project_mgr->active();
    if (!project) return PlayPresentation::docked;
    return project->config().play.fullscreen
        ? PlayPresentation::fullscreen : PlayPresentation::docked;
}

void PlayController::toggle_presentation() {
    if (game_mode_) return;
    docked_override_ = false;
    auto* project = s_.project_mgr->active();
    if (!project) return;
    project->config().play.fullscreen = !project->config().play.fullscreen;
    project->save();
}

void PlayController::toggle_eject() {
    if (!ejected_ && presentation() == PlayPresentation::fullscreen) {
        docked_override_ = true;
    }
    ejected_ = !ejected_;
    if (ejected_) {
        release_input();
    } else {
        // Possessing hands input back to the game — without this,
        // re-possessing only looked like it worked when the eject/possess
        // button itself sat inside the viewport image rect, which
        // check_viewport_recapture_click() treats as the player clicking
        // back in; toggling from the keyboard (or anywhere else the
        // button isn't) left the game correctly unfocused per that same
        // logic, with nothing left to click to recapture it.
        captured_ = true;
    }
}

void PlayController::begin_frame() {
    // Push this frame's focus state before anything reads through Input's
    // gated query methods — ImGui's own NewFrame (and therefore every
    // editor widget/shortcut check) runs later in Engine::run, so setting
    // it here guarantees it is never one frame stale for either side.
    const bool focused = input_focused();
    s_.input->set_game_focus(focused);

    // A cursor locked for mouse-look has to come unlocked the moment the
    // editor takes focus back, or there is no usable pointer for the chrome
    // around the game — and lock again by itself when focus returns, since
    // the script that asked never sees focus change.
    s_.input->apply_cursor_lock(focused);

    // This frame's button edges, kept for the next fixed step that begins,
    // however many frames that is from now.
    if (playing()) {
        for (const auto& [action, state] : s_.input_map->states()) {
            step_input_.record(action, state.pressed, state.released);
        }
    }

    if (!stepping_) return;
    stepping_ = false;
    // The frame this unlocked is about to tick; re-pause so the next one
    // doesn't advance too, without ever broadcasting resume/pause for what
    // is really a single-frame nudge.
    s_.clock->set_paused(true);
    // However long the frame took, the simulation moves by one step of it.
    fixed_timestep_.request_single_step();
}

bool PlayController::input_focused() const {
    if (!playing()) return false;
    return presentation() == PlayPresentation::fullscreen || captured_;
}

void PlayController::capture_input() {
    if (!playing() || presentation() == PlayPresentation::fullscreen) return;
    captured_ = true;
}

void PlayController::release_input() {
    captured_ = false;
    if (s_.input->is_cursor_captured()) {
        s_.input->set_cursor_captured(false);
    }
}

void PlayController::flush_spawned() {
    // Every node that joined the game since the last flush is started here,
    // however it joined: spawned during a tick (deferred until now), or added
    // directly from an input handler, a collision callback, a UI action, a
    // preset. Starting a round's nodes runs their begin_play, which can spawn
    // more; those are the next round. A hard bound on rounds so a script that
    // spawns from every begin_play stalls with a warning instead of hanging.
    constexpr int MAX_ROUNDS = 64;
    for (int round = 0;; ++round) {
        (void)s_.scene->flush_spawned();
        if (!s_.scene->has_added()) return;
        if (round == MAX_ROUNDS) {
            FJELL_CORE_WARN("PlayController: nodes still joining after {} rounds, a begin_play "
                            "keeps spawning; leaving the rest for the next flush",
                            MAX_ROUNDS);
            return;
        }
        const std::vector<uint32_t> joined = s_.scene->take_added();
        // A node may carry an InputComponent (e.g. a player's camera); load
        // its contexts before begin_play so scripts can bind/activate input.
        load_input_contexts();
        // Bodies before begin_play: a node's script may query physics or set a
        // velocity in its first frame, and a body that appears a tick later
        // means the object falls through whatever it spawned on.
        if (auto* physics = s_.physics->world()) {
            for (uint32_t id : joined) {
                if (auto* node = s_.scene->find(id)) physics->create_body(*s_.scene, *node);
            }
        }
        s_.scripts->manager().initialize_spawned(*s_.scene, joined);
    }
}

void PlayController::load_input_contexts() {
    auto* project = s_.project_mgr->active();
    for (const auto& obj : s_.scene->objects()) {
        const auto* ic = obj->components.get<InputComponent>();
        if (!ic) continue;

        auto actions_dir = ic->actions_dir;
        if (actions_dir.empty()) actions_dir = "assets/input/actions";
        if (project) {
            s_.input_map->load_actions_dir(project->resolve(actions_dir));
        }

        for (const auto& ctx_path : ic->context_paths) {
            std::string resolved = project ? project->resolve(ctx_path) : ctx_path;
            s_.input_map->load_context(resolved);
            s_.input_map->push_context(std::filesystem::path(ctx_path).stem().string());
        }
    }
}

void PlayController::apply_pending_hud_resize() {
    if (hud_pending_destroy_) {
        // begin_frame()'s fence wait (already run by the time Engine::run
        // calls this) only guarantees the GPU is done with the *current*
        // frame-in-flight slot. With more than one frame pipelined at once,
        // hud_target_'s descriptor may have last been referenced by a draw
        // recorded into a *different* slot's command buffer — confirmed by
        // hitting this exact free from gdb and seeing the validation layer
        // reject it as still in use by graphics_post[2] while begin_frame
        // had only fenced a different slot. A full wait_idle is the same
        // price stop() and UiSubsystem::on_play_stop already pay before
        // tearing down GameUi pipelines, for the same reason.
        s_.vulkan->wait_idle();
        hud_target_.reset();
        hud_pending_destroy_ = false;
    }

    if (!playing()) return;
    if (hud_img_size_.x <= 0.0f || hud_img_size_.y <= 0.0f) return;

    auto w = static_cast<uint32_t>(hud_img_size_.x);
    auto h = static_cast<uint32_t>(hud_img_size_.y);
    if (!hud_target_) {
        hud_target_ = std::make_unique<OffscreenPass>(
            s_.vulkan->device_ref(), s_.vulkan->allocator(), w, h, VK_SAMPLE_COUNT_1_BIT);
    } else {
        // Unlike the destroy branch above, this was never the vulnerable
        // path: OffscreenPass::resize() already calls vkDeviceWaitIdle
        // itself before touching anything, and only when the size actually
        // changes (an early-return no-op otherwise).
        hud_target_->resize(w, h);
    }
}

void PlayController::record_hud(uint32_t frame_index) {
    if (!playing() || !hud_target_) return;

    s_.vulkan->record_game_ui_offscreen_multi(
        [this](VkCommandBuffer cmd, VkDescriptorSet global_set,
              VkDescriptorSet bindless_set, VkExtent2D extent, uint32_t idx) {
            for (auto& inst : s_.ui->instances()) {
                if (inst.render_mode == UiWidgetComponent::RenderMode::world_space) continue;
                if (!inst.game_ui || !inst.game_ui->loaded()) continue;
                // viewport_offset translates raw (window-absolute) mouse
                // coordinates into the HUD target's own local space; the
                // target itself is already sized to the panel, so the
                // offset is the panel's own window-absolute top-left.
                inst.game_ui->update(hud_img_size_.x, hud_img_size_.y, idx,
                                     s_.clock->delta_time(), s_.input, hud_img_pos_);
                inst.game_ui->record(cmd, global_set, bindless_set, extent, idx);
            }
        },
        *hud_target_, frame_index);
}

VkDescriptorSet PlayController::hud_texture() {
    if (!hud_target_) return VK_NULL_HANDLE;
    return hud_target_->imgui_texture();
}

void PlayController::start() {
    if (state_ != PlayState::stopped) return;

    // Docked play starts focused immediately — see input_focused(); the
    // member's own default already covers this, this just guards against a
    // stray Shift+Esc from a previous session somehow surviving to here.
    // Meaningless in fullscreen, where input_focused() short-circuits to
    // true regardless.
    captured_ = true;
    ejected_ = false;
    docked_override_ = false;

    std::optional<PlaySnapshot> snapshot;
    if (game_mode_) {
        // The script subsystem builds its play instances from each node's
        // serialized JSON, so the values have to be current even when there
        // is nothing to restore afterwards.
        s_.scripts->manager().serialize_all(*s_.scene);
    } else {
        snapshot = PlaySnapshot::capture(s_);
    }

    // The scene's nodes as they stand are started below, all at once. What
    // joins from here on, by whatever path, flush_spawned() starts.
    s_.scene->track_added(true);

    // Physics and VFX start first and without a physics world in their
    // context: nothing else depends on them, and everything after needs the
    // physics world to exist.
    {
        TickContext ctx{
            .scene = *s_.scene,
            .input = *s_.input,
            .clock = *s_.clock,
            .dt = s_.clock->delta_time(),
            .physics = nullptr,
            .project = s_.project_mgr->active(),
        };
        s_.physics->notify_play_start(ctx);
        s_.vfx->notify_play_start(ctx);
    }

    {
        TickContext ctx{
            .scene = *s_.scene,
            .input = *s_.input,
            .clock = *s_.clock,
            .dt = s_.clock->delta_time(),
            .physics = s_.physics->world(),
            .project = s_.project_mgr->active(),
        };
        s_.camera->notify_play_start(ctx);
        s_.audio->notify_play_start(ctx);
        s_.sprite_anim->notify_play_start(ctx);
        s_.environment->notify_play_start(ctx);

        // Contexts before scripts: begin_play binds actions through input::bind.
        load_input_contexts();

        // A previous session's begin_play left bindings that captured `this`
        // pointers to instances the script subsystem is about to replace.
        fjell_input_clear_connections();

        s_.scripts->notify_play_start(ctx);

        // begin_play may have spawned nodes (a server spawning each player
        // and its camera). They exist only as a pending queue until flushed;
        // doing it here gives them their input contexts, bodies and scripts,
        // and lets the UI widgets starting next find them by lookup.
        flush_spawned();

        s_.ui->notify_play_start(ctx);
    }

    // The simulation starts where game time is, so anything that follows
    // game time (the water shaders) and the simulation agree from the first
    // step. A frame counts for no more game time than the steps it may run.
    const Project* project = s_.project_mgr->active();
    const std::uint32_t tick_rate = project != nullptr ? project->config().simulation.fixed_tick_rate
                                                       : FixedTimestep::DEFAULT_TICK_RATE;
    s_.scene->set_gone_remembered(project != nullptr ? project->config().simulation.gone_nodes_remembered
                                                     : ProjectConfig::SimulationConfig{}.gone_nodes_remembered);
    fixed_timestep_.reset(1.0 / tick_rate, s_.clock->game_elapsed());
    step_stats_ = {};
    step_stats_.tick_rate = tick_rate;
    step_input_.reset();
    s_.clock->set_max_game_delta(static_cast<float>(fixed_timestep_.max_frame_seconds()));

    session_ = std::make_unique<PlaySession>(std::move(snapshot));
    state_ = PlayState::playing;
    s_.vulkan->set_playing(true);
    s_.game_ui->mark_dirty();
    FJELL_CORE_INFO("Play mode started");
    on_started.broadcast();
}

void PlayController::stop() {
    if (state_ == PlayState::stopped) return;

    if (game_mode_) {
        s_.window->request_close();
        return;
    }

    on_stopping.broadcast();

    TickContext ctx{
        .scene = *s_.scene,
        .input = *s_.input,
        .clock = *s_.clock,
        .dt = s_.clock->delta_time(),
        .physics = s_.physics->world(),
        .project = s_.project_mgr->active(),
    };

    s_.clock->set_time_scale(1.0f);
    s_.clock->set_paused(false);
    s_.clock->clear_max_game_delta();

    fjell_input_clear_connections();
    s_.ui->notify_play_stop(ctx);
    s_.camera->notify_play_stop(ctx);
    s_.audio->notify_play_stop(ctx);
    s_.sprite_anim->notify_play_stop(ctx);
    s_.environment->notify_play_stop(ctx);
    s_.scripts->notify_play_stop(ctx);
    s_.vfx->notify_play_stop(ctx);

    session_->snapshot()->restore(s_);
    s_.scene->track_added(false);

    // The editor draws every node where its transform puts it.
    s_.scene->clear_interpolation();

    // Bodies are keyed by node id, so the world can outlive the play scene
    // by a moment without referencing anything that is gone.
    s_.physics->notify_play_stop(ctx);

    // Not destroyed here — see hud_pending_destroy_'s comment. Deferring
    // costs one frame of an unused OffscreenPass sitting around, which is
    // fine; destroying it early risks a validation error every time stop()
    // runs mid-draw (the toolbar's Stop button) rather than only from a
    // shortcut evaluated before any drawing starts.
    hud_pending_destroy_ = true;

    session_.reset();
    state_ = PlayState::stopped;
    step_stats_ = {};
    s_.vulkan->set_playing(false);
    // The request dies with the session that made it: scripts are gone, so
    // nothing is left to ask again, and a stale one would start the next
    // session with the cursor already locked.
    s_.input->set_cursor_lock_wanted(false);
    if (s_.input->is_cursor_captured()) {
        s_.input->set_cursor_captured(false);
    }
    FJELL_CORE_INFO("Play mode stopped");
    on_stopped.broadcast();
}

void PlayController::shutdown() {
    if (!hud_target_) return;
    s_.vulkan->wait_idle();
    hud_target_.reset();
}

} // namespace fjell
