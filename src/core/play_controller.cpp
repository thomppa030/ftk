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
#include "core/window.hpp"
#include "audio/audio_subsystem.hpp"
#include "physics/physics_subsystem.hpp"
#include "renderer/systems/sprite_anim_subsystem.hpp"
#include "renderer/systems/vfx_subsystem.hpp"
#include "renderer/vulkan_context.hpp"
#include "scene/camera_subsystem.hpp"
#include "scene/component_registry.hpp"
#include "scene/scene.hpp"
#include "scripting/script_subsystem.hpp"
#include "ui/game_ui/game_ui.hpp"
#include "ui/game_ui/ui_subsystem.hpp"

#include <GLFW/glfw3.h>

#include <filesystem>

namespace fjell {

PlayController::PlayController() = default;
PlayController::~PlayController() = default;

void PlayController::init(PlayServices services, bool game_mode) {
    s_ = services;
    game_mode_ = game_mode;
    check_component_serialization();
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

void PlayController::start() {
    if (state_ != PlayState::stopped) return;

    std::optional<PlaySnapshot> snapshot;
    if (game_mode_) {
        // The script subsystem builds its play instances from each node's
        // serialized JSON, so the values have to be current even when there
        // is nothing to restore afterwards.
        s_.scripts->manager().serialize_all(*s_.scene);
    } else {
        snapshot = PlaySnapshot::capture(s_);
    }

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

        // Contexts before scripts: begin_play binds actions through InputAPI.
        load_input_contexts();

        // A previous session's begin_play left bindings that captured `this`
        // pointers to instances the script subsystem is about to replace.
        fjell_input_clear_connections();

        s_.scripts->notify_play_start(ctx);
        s_.ui->notify_play_start(ctx);

        // begin_play may have spawned nodes carrying InputComponents (a
        // server spawning each player and its camera). Their binds already
        // attached to the actions; loading the contexts enables dispatch.
        load_input_contexts();
    }

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
        glfwSetWindowShouldClose(s_.window->handle(), GLFW_TRUE);
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

    fjell_input_clear_connections();
    s_.ui->notify_play_stop(ctx);
    s_.camera->notify_play_stop(ctx);
    s_.audio->notify_play_stop(ctx);
    s_.sprite_anim->notify_play_stop(ctx);
    s_.scripts->notify_play_stop(ctx);
    s_.vfx->notify_play_stop(ctx);

    session_->snapshot()->restore(s_);

    // Bodies are keyed by node id, so the world can outlive the play scene
    // by a moment without referencing anything that is gone.
    s_.physics->notify_play_stop(ctx);

    session_.reset();
    state_ = PlayState::stopped;
    s_.vulkan->set_playing(false);
    if (s_.input->is_cursor_captured()) {
        s_.input->set_cursor_captured(s_.window->handle(), false);
    }
    FJELL_CORE_INFO("Play mode stopped");
    on_stopped.broadcast();
}

} // namespace fjell
