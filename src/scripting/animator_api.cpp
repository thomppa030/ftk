#include "scripting/script_globals.hpp"
#include "animation/animator_component.hpp"
#include "scene/scene.hpp"

static fjell::AnimatorComponent* find_animator(uint32_t node_id) {
    auto* scene = fjell::script_active_scene();
    if (!scene) return nullptr;
    auto* node = scene->find(node_id);
    if (!node) return nullptr;
    return node->components.get<fjell::AnimatorComponent>();
}

extern "C" void fjell_animator_set_param(uint32_t node_id, const char* name, float value) {
    auto* anim = find_animator(node_id);
    if (anim && name) anim->set_param(name, value);
}

extern "C" float fjell_animator_get_param(uint32_t node_id, const char* name) {
    auto* anim = find_animator(node_id);
    if (anim && name) return anim->get_param(name);
    return 0.0f;
}

extern "C" const char* fjell_animator_current_state(uint32_t node_id) {
    auto* anim = find_animator(node_id);
    if (anim) return anim->state_machine().current_state_name().c_str();
    return "";
}
