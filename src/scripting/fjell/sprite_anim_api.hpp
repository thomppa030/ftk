// sprite_anim_api.hpp — Sprite animation API for game scripts
#pragma once

#include "fjell/api.h"

#include <cstdint>

extern "C" {
FJELL_SDK_EXPORT void  fjell_sprite_anim_set_param(uint32_t node_id, const char* name, float value);
FJELL_SDK_EXPORT float fjell_sprite_anim_get_param(uint32_t node_id, const char* name);
FJELL_SDK_EXPORT const char* fjell_sprite_anim_current_state(uint32_t node_id);
FJELL_SDK_EXPORT void  fjell_sprite_anim_set_flip_h(uint32_t node_id, bool flip);
FJELL_SDK_EXPORT bool  fjell_sprite_anim_is_finished(uint32_t node_id);
FJELL_SDK_EXPORT void  fjell_sprite_anim_set_playing(uint32_t node_id, bool playing);
}

namespace fjell {

struct SpriteAnim {
    static void set_param(uint32_t node_id, const char* name, float value) {
        fjell_sprite_anim_set_param(node_id, name, value);
    }

    static float get_param(uint32_t node_id, const char* name) {
        return fjell_sprite_anim_get_param(node_id, name);
    }

    static const char* current_state(uint32_t node_id) {
        return fjell_sprite_anim_current_state(node_id);
    }

    static void set_flip_h(uint32_t node_id, bool flip) {
        fjell_sprite_anim_set_flip_h(node_id, flip);
    }

    static bool is_finished(uint32_t node_id) {
        return fjell_sprite_anim_is_finished(node_id);
    }

    static void set_playing(uint32_t node_id, bool playing) {
        fjell_sprite_anim_set_playing(node_id, playing);
    }
};

} // namespace fjell
