#include "renderer/systems/sprite_anim_subsystem.hpp"
#include "scripting/script_call.hpp"
#include "renderer/components/sprite_anim_component.hpp"
#include "renderer/components/sprite_desc.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/resources/texture.hpp"
#include "core/base_path.hpp"
#include "core/frame_context.hpp"
#include "ftk/base/log.hpp"
#include "ftk/base/profiler.hpp"
#include "scene/scene.hpp"

#include <filesystem>

namespace fjell {

static void load_anim_data(SpriteAnimComponent& anim, ResourceManager& resources,
                           const Project* project) {
    // Load via .fjanimset if available
    if (!anim.animset_path.empty()) {
        std::string resolved = base_path::resolve_asset(anim.animset_path, project);
        if (std::filesystem::exists(resolved)) {
            if (auto r = anim.state_machine.load(resolved); !r) {
                FTK_CORE_WARN("SpriteAnim: failed to load animset {}: {}", anim.animset_path, r.error());
            }
        }
    } else if (!anim.animations.empty()) {
        // Legacy: build state machine from direct animation map
        // Not using .fjanimset — just set initial state
        if (!anim.default_animation.empty()) {
            // Will be handled below when loading sheets
        }
    }

    // Determine which clip paths to load:
    // Either from state machine states, or from legacy animations map
    std::unordered_map<std::string, std::string> clips_to_load;

    if (!anim.state_machine.states().empty()) {
        for (auto& state : anim.state_machine.states()) {
            clips_to_load[state.name] = state.clip;
        }
    } else {
        clips_to_load = anim.animations;
    }

    // Load each sheet
    for (auto& [name, sheet_path] : clips_to_load) {
        std::string resolved = base_path::resolve_asset(sheet_path, project);
        if (!std::filesystem::exists(resolved)) continue;

        auto sheet = load_sprite_sheet(resolved);
        if (sheet.texture_path.empty()) continue;

        std::string tex_resolved = std::filesystem::path(sheet.texture_path).is_absolute()
            ? sheet.texture_path
            : base_path::resolve_asset(sheet.texture_path, project);

        auto tex_result = resources.load_texture_nearest(tex_resolved);
        if (!tex_result) continue;

        auto* texture = resources.get(*tex_result);
        if (!texture) continue;

        compute_frame_uvs(sheet, texture->width(), texture->height());
        anim.sheets[name] = std::move(sheet);
    }

    anim.loaded = true;
}

// Apply a sheet's frame to the sprite component
static void apply_frame(SpriteComponent& sprite, const SpriteSheet& sheet, uint32_t frame) {
    if (frame < sheet.frame_uvs.size()) {
        sprite.uv_rect = sheet.frame_uvs[frame];
    }
    if (sprite.texture_path != sheet.texture_path) {
        sprite.texture_path = sheet.texture_path;
        sprite.texture_loaded = false;
    }
}

// Get the active sheet for the current state
static const SpriteSheet* active_sheet(const SpriteAnimComponent& anim) {
    // Try state machine current state
    if (!anim.state_machine.states().empty()) {
        auto it = anim.sheets.find(anim.state_machine.current_state_name());
        if (it != anim.sheets.end()) return &it->second;
    }
    // Legacy: try default or first
    if (!anim.default_animation.empty()) {
        auto it = anim.sheets.find(anim.default_animation);
        if (it != anim.sheets.end()) return &it->second;
    }
    if (!anim.sheets.empty()) {
        return &anim.sheets.begin()->second;
    }
    return nullptr;
}

void SpriteAnimSubsystem::on_play_start(const TickContext& ctx) {
    for (auto [obj, anim] : ctx.scene.view<SpriteAnimComponent>()) {

        if (!anim->loaded) {
            load_anim_data(*anim, *resources_, ctx.project);
        }

        anim->current_frame = 0;
        anim->frame_timer = 0.0f;
        anim->playing = true;
        anim->finished = false;

        // Apply first frame
        auto* sprite = obj->components.get<SpriteComponent>();
        if (!sprite) continue;

        const auto* sheet = active_sheet(*anim);
        if (sheet) apply_frame(*sprite, *sheet, 0);
    }
}

void SpriteAnimSubsystem::tick(const TickContext& ctx) {
    FTK_PROFILE_SCOPE_N("sprite_anim_tick");

    for (auto [obj, anim] : ctx.scene.view<SpriteAnimComponent>()) {

        auto* sprite = obj->components.get<SpriteComponent>();
        if (!sprite) continue;

        // Lazy-load
        if (!anim->loaded) {
            load_anim_data(*anim, *resources_, ctx.project);
        }

        const auto* sheet = active_sheet(*anim);
        if (!sheet || sheet->frame_uvs.empty()) continue;

        // Editor mode: show first frame
        if (!playing()) {
            apply_frame(*sprite, *sheet, 0);
            continue;
        }

        // Play mode: evaluate state machine transitions
        if (!anim->state_machine.states().empty()) {
            std::string prev_state = anim->state_machine.current_state_name();
            // A transition tells the machine's listeners, which may be game code.
            const ScriptCall call{.type_name = "A sprite animation's state listener", .node = obj};
            call_script(call, [&] { anim->state_machine.update(ctx.dt); });

            // If state changed, reset frame and switch sheet
            if (anim->state_machine.current_state_name() != prev_state) {
                anim->current_frame = 0;
                anim->frame_timer = 0.0f;
                anim->finished = false;
                sheet = active_sheet(*anim);
                if (!sheet || sheet->frame_uvs.empty()) continue;
            }
        }

        if (!anim->playing || anim->finished) {
            apply_frame(*sprite, *sheet, anim->current_frame);
            continue;
        }

        // Advance frame timer
        float speed = anim->playback_speed;
        if (!anim->state_machine.states().empty()) {
            auto* state = anim->state_machine.current_state();
            if (state) speed *= state->speed;
        }

        anim->frame_timer += ctx.dt * speed;
        float frame_duration = 1.0f / sheet->fps;

        while (anim->frame_timer >= frame_duration) {
            anim->frame_timer -= frame_duration;
            anim->current_frame++;

            if (anim->current_frame >= sheet->frame_count) {
                if (sheet->loop) {
                    anim->current_frame = 0;
                } else {
                    anim->current_frame = sheet->frame_count - 1;
                    anim->finished = true;
                    break;
                }
            }
        }

        apply_frame(*sprite, *sheet, anim->current_frame);
    }
}

} // namespace fjell
