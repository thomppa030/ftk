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

#include <GLFW/glfw3.h>

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
    auto* project = s_.project_mgr->active();
    if (!project) return PlayPresentation::docked;
    return project->config().play.fullscreen
        ? PlayPresentation::fullscreen : PlayPresentation::docked;
}

void PlayController::toggle_presentation() {
    if (game_mode_) return;
    auto* project = s_.project_mgr->active();
    if (!project) return;
    project->config().play.fullscreen = !project->config().play.fullscreen;
    project->save();
}

void PlayController::begin_frame() {
    if (!stepping_) return;
    stepping_ = false;
    // The frame this unlocked is about to tick; re-pause so the next one
    // doesn't advance too, without ever broadcasting resume/pause for what
    // is really a single-frame nudge.
    s_.clock->set_paused(true);
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

    // Not destroyed here — see hud_pending_destroy_'s comment. Deferring
    // costs one frame of an unused OffscreenPass sitting around, which is
    // fine; destroying it early risks a validation error every time stop()
    // runs mid-draw (the toolbar's Stop button) rather than only from a
    // shortcut evaluated before any drawing starts.
    hud_pending_destroy_ = true;

    session_.reset();
    state_ = PlayState::stopped;
    s_.vulkan->set_playing(false);
    if (s_.input->is_cursor_captured()) {
        s_.input->set_cursor_captured(s_.window->handle(), false);
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
