#include "fjell/api.h"
#include "scripting/script_globals.hpp"
#include "renderer/components/sprite_anim_component.hpp"
#include "renderer/components/sprite_desc.hpp"
#include "scene/scene.hpp"

static fjell::SpriteAnimComponent* find_anim(uint32_t node_id) {
    auto* scene = fjell::script_services().scene;
    if (!scene) return nullptr;
    auto* node = scene->find(node_id);
    if (!node) return nullptr;
    return node->components.get<fjell::SpriteAnimComponent>();
}

static fjell::SpriteComponent* find_sprite(uint32_t node_id) {
    auto* scene = fjell::script_services().scene;
    if (!scene) return nullptr;
    auto* node = scene->find(node_id);
    if (!node) return nullptr;
    return node->components.get<fjell::SpriteComponent>();
}

extern "C" FJELL_SDK_EXPORT void fjell_sprite_anim_set_param(uint32_t node_id, const char* name, float value) {
    auto* anim = find_anim(node_id);
    if (anim && name) anim->state_machine.set_param(name, value);
}

extern "C" FJELL_SDK_EXPORT float fjell_sprite_anim_get_param(uint32_t node_id, const char* name) {
    auto* anim = find_anim(node_id);
    if (anim && name) return anim->state_machine.get_param(name);
    return 0.0f;
}

extern "C" FJELL_SDK_EXPORT const char* fjell_sprite_anim_current_state(uint32_t node_id) {
    auto* anim = find_anim(node_id);
    if (anim) return anim->state_machine.current_state_name().c_str();
    return "";
}

extern "C" FJELL_SDK_EXPORT void fjell_sprite_anim_set_flip_h(uint32_t node_id, bool flip) {
    auto* sprite = find_sprite(node_id);
    if (sprite) sprite->flip_h = flip;
}

extern "C" FJELL_SDK_EXPORT bool fjell_sprite_anim_is_finished(uint32_t node_id) {
    auto* anim = find_anim(node_id);
    return anim ? anim->finished : false;
}

extern "C" FJELL_SDK_EXPORT void fjell_sprite_anim_set_playing(uint32_t node_id, bool playing) {
    auto* anim = find_anim(node_id);
    if (anim) anim->playing = playing;
}
