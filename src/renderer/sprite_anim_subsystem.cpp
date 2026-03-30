#include "renderer/sprite_anim_subsystem.hpp"
#include "renderer/sprite_anim_component.hpp"
#include "renderer/sprite_desc.hpp"
#include "renderer/resource_manager.hpp"
#include "renderer/texture.hpp"
#include "core/base_path.hpp"
#include "core/frame_context.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "scene/scene.hpp"

#include <filesystem>

namespace fjell {

static void load_sheets(SpriteAnimComponent& anim, ResourceManager& resources,
                        const Project* project) {
    for (auto& [name, sheet_path] : anim.animations) {
        std::string resolved = base_path::resolve_asset(sheet_path, project);
        if (!std::filesystem::exists(resolved)) {
            FJELL_CORE_WARN("SpriteAnim: sheet not found: {} -> {}", sheet_path, resolved);
            continue;
        }

        auto sheet = load_sprite_sheet(resolved);

        // Resolve the texture path relative to the .fjsheet file's directory
        std::string tex_resolved;
        if (sheet.texture_path.empty()) {
            FJELL_CORE_WARN("SpriteAnim: sheet '{}' has empty texture path", name);
            continue;
        }
        if (std::filesystem::path(sheet.texture_path).is_absolute()) {
            tex_resolved = sheet.texture_path;
        } else {
            // Texture path in .fjsheet is relative to the project
            tex_resolved = base_path::resolve_asset(sheet.texture_path, project);
        }

        // Load the texture to get dimensions
        auto tex_result = resources.load_texture_nearest(tex_resolved);
        if (!tex_result) {
            FJELL_CORE_WARN("SpriteAnim: failed to load texture: {}", tex_resolved);
            continue;
        }

        auto* texture = resources.get(*tex_result);
        if (!texture) continue;

        compute_frame_uvs(sheet, texture->width(), texture->height());
        FJELL_CORE_INFO("SpriteAnim: loaded '{}' — {}x{}, {} frames, {} uvs",
                        name, texture->width(), texture->height(),
                        sheet.frame_count, sheet.frame_uvs.size());
        anim.sheets[name] = std::move(sheet);
    }
    anim.sheets_loaded = true;
}

void SpriteAnimSubsystem::on_play_start(const TickContext& ctx) {
    // Set up initial animation state for all anim components
    for (auto& obj : ctx.scene.objects()) {
        auto* anim = obj->components.get<SpriteAnimComponent>();
        if (!anim) continue;

        if (!anim->sheets_loaded) {
            load_sheets(*anim, *resources_, ctx.project);
        }

        // Start the default animation
        if (!anim->default_animation.empty()) {
            anim->current_animation = anim->default_animation;
        } else if (!anim->animations.empty()) {
            anim->current_animation = anim->animations.begin()->first;
        }

        anim->current_frame = 0;
        anim->frame_timer = 0.0f;
        anim->playing = true;
        anim->finished = false;

        FJELL_CORE_INFO("SpriteAnim: current='{}', sheets={}, anims={}",
                        anim->current_animation, anim->sheets.size(), anim->animations.size());

        // Apply first frame to sprite
        auto* sprite = obj->components.get<SpriteComponent>();
        if (!sprite) {
            FJELL_CORE_WARN("SpriteAnim: no SpriteComponent on node '{}'", obj->name);
            continue;
        }

        auto sheet_it = anim->sheets.find(anim->current_animation);
        if (sheet_it == anim->sheets.end() || sheet_it->second.frame_uvs.empty()) {
            FJELL_CORE_WARN("SpriteAnim: sheet not found for '{}' (have {} sheets)",
                            anim->current_animation, anim->sheets.size());
            for (auto& [k, v] : anim->sheets)
                FJELL_CORE_WARN("  sheet key: '{}'", k);
            continue;
        }

        auto& sheet = sheet_it->second;
        sprite->uv_rect = sheet.frame_uvs[0];
        FJELL_CORE_INFO("SpriteAnim: play_start '{}' frame 0 uv=[{},{},{},{}] tex={}",
                        anim->current_animation,
                        sprite->uv_rect.x, sprite->uv_rect.y,
                        sprite->uv_rect.z, sprite->uv_rect.w,
                        sheet.texture_path);

        // Set the texture from the sheet if sprite doesn't have one yet
        if (sprite->texture_path != sheet.texture_path) {
            sprite->texture_path = sheet.texture_path;
            sprite->texture_loaded = false;
        }
    }
}

void SpriteAnimSubsystem::tick(const TickContext& ctx) {
    FJELL_PROFILE_SCOPE_N("sprite_anim_tick");

    for (auto& obj : ctx.scene.objects()) {
        auto* anim = obj->components.get<SpriteAnimComponent>();
        if (!anim || !anim->playing || anim->finished) continue;

        auto* sprite = obj->components.get<SpriteComponent>();
        if (!sprite) continue;

        // Lazy-load sheets if not done yet
        if (!anim->sheets_loaded) {
            load_sheets(*anim, *resources_, ctx.project);
        }

        auto sheet_it = anim->sheets.find(anim->current_animation);
        if (sheet_it == anim->sheets.end()) continue;
        auto& sheet = sheet_it->second;

        if (sheet.frame_uvs.empty()) continue;

        // Advance timer
        anim->frame_timer += ctx.dt * anim->playback_speed;
        float frame_duration = 1.0f / sheet.fps;

        while (anim->frame_timer >= frame_duration) {
            anim->frame_timer -= frame_duration;
            anim->current_frame++;

            if (anim->current_frame >= sheet.frame_count) {
                if (sheet.loop) {
                    anim->current_frame = 0;
                } else {
                    anim->current_frame = sheet.frame_count - 1;
                    anim->finished = true;
                    break;
                }
            }
        }

        // Update sprite UV rect
        sprite->uv_rect = sheet.frame_uvs[anim->current_frame];

        // Ensure the correct texture is set
        if (sprite->texture_path != sheet.texture_path) {
            sprite->texture_path = sheet.texture_path;
            sprite->texture_loaded = false;
        }
    }
}

} // namespace fjell
