#include "fjell/api.h"
#include "scripting/script_globals.hpp"
#include "scripting/script_manager.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "scene/preset_asset.hpp"
#include "scene/scene.hpp"
#include "scene/component_registry.hpp"

extern "C" FJELL_SDK_EXPORT uint32_t fjell_scene_find_node(const char* name) {
    auto* scene = fjell::script_services().scene;
    if (!scene || !name) return 0;
    for (const auto& obj : scene->objects()) {
        if (obj->name == name) return obj->id;
    }
    return 0;
}

extern "C" FJELL_SDK_EXPORT void fjell_scene_destroy(uint32_t node_id) {
    auto* scene = fjell::script_services().scene;
    if (scene) scene->queue_destroy(node_id);
}

extern "C" FJELL_SDK_EXPORT float* fjell_scene_get_position(uint32_t node_id) {
    auto* scene = fjell::script_services().scene;
    if (!scene) return nullptr;
    auto* node = scene->find_spatial(node_id);
    return node ? &node->transform.position.x : nullptr;
}

extern "C" FJELL_SDK_EXPORT float* fjell_scene_get_scale(uint32_t node_id) {
    auto* scene = fjell::script_services().scene;
    if (!scene) return nullptr;
    auto* node = scene->find_spatial(node_id);
    return node ? &node->transform.scale.x : nullptr;
}

extern "C" FJELL_SDK_EXPORT float* fjell_scene_get_rotation(uint32_t node_id) {
    auto* scene = fjell::script_services().scene;
    if (!scene) return nullptr;
    auto* node = scene->find_spatial(node_id);
    return node ? &node->transform.rotation.x : nullptr;
}

extern "C" FJELL_SDK_EXPORT void* fjell_scene_get_component(uint32_t node_id, const char* type_name) {
    auto* scene = fjell::script_services().scene;
    if (!scene || !type_name) return nullptr;
    auto* node = scene->find(node_id);
    if (!node) return nullptr;

    // Engine components — registry dispatch
    auto* meta = fjell::ComponentRegistry::instance().find_by_script_name(type_name);
    if (meta && meta->script_get) {
        return meta->script_get(*node);
    }

    // Script components
    for (auto& entry : node->scripts) {
        if (entry.instance && entry.type_name == type_name)
            return entry.instance.get();
    }
    return nullptr;
}

extern "C" FJELL_SDK_EXPORT uint32_t fjell_scene_spawn_empty(const char* name,
                                            float x, float y, float z) {
    auto* scene = fjell::script_services().scene;
    if (!scene) return 0;
    fjell::Transform t;
    t.position = {x, y, z};
    return scene->add_empty(t, name ? name : "Spawned");
}

extern "C" FJELL_SDK_EXPORT void* fjell_scene_add_component(uint32_t node_id,
                                           const char* type_name) {
    auto* scene = fjell::script_services().scene;
    if (!scene || !type_name) return nullptr;
    auto* node = scene->find(node_id);
    if (!node) return nullptr;

    // Engine components — registry dispatch
    auto* meta = fjell::ComponentRegistry::instance().find_by_script_name(type_name);
    if (meta && meta->script_add) {
        return meta->script_add(*node);
    }

    // Script components
    auto* mgr = fjell::script_services().manager;
    auto* spatial = node->as_spatial();
    if (!spatial || !mgr) return nullptr;
    auto result = mgr->add_script(*spatial, type_name);
    if (!result) return nullptr;
    auto& scripts = spatial->scripts;
    if (!scripts.empty() && scripts.back().instance)
        return scripts.back().instance.get();
    return nullptr;
}

extern "C" FJELL_SDK_EXPORT uint32_t fjell_scene_spawn_preset(uint32_t handle_id,
                                                              float x, float y, float z,
                                                              int32_t parent_id) {
    auto& s = fjell::script_services();
    if (!s.scene || !s.resources) return 0;

    auto* asset = s.resources->get(fjell::PresetAssetHandle{handle_id});
    if (!asset) return 0;

    fjell::Transform offset;
    offset.position = {x, y, z};
    return asset->preset.instantiate(*s.scene, parent_id, offset);
}
