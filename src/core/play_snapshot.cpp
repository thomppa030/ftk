#include "core/play_snapshot.hpp"

#include "core/input_action_map.hpp"
#include "core/play_controller.hpp"
#include "core/project_manager.hpp"
#include "ftk/base/log.hpp"
#include "renderer/vulkan_context.hpp"
#include "scene/scene.hpp"
#include "scripting/screen_debug_queue.hpp"
#include "scripting/script_subsystem.hpp"

#include <algorithm>

namespace fjell {

PlaySnapshot PlaySnapshot::capture(const PlayServices& s) {
    // Each node's serialized script JSON is only refreshed on demand, so
    // inspector edits made since the scene was loaded live in the script
    // instances alone until it is.
    s.scripts->manager().serialize_all(*s.scene);

    PlaySnapshot snap;
    snap.scene = s.scene->to_json(s.project_mgr->active());
    snap.scene_dirty = s.scene->is_dirty();
    snap.input_contexts = s.input_map->context_stack();
    snap.viewports = s.vulkan->scene_viewport_names();
    return snap;
}

void PlaySnapshot::restore(const PlayServices& s) const {
    // Same guard as a scene load: the frame in flight may still reference
    // per-node GPU state.
    s.vulkan->wait_idle();

    // The restored scene is a new scene to the particle system, so the
    // play scene's emitters have to go first or they stay behind frozen.
    s.vulkan->release_scene_particles(*s.scene);
    *s.scene = Scene::from_json(scene, *s.resources, s.project_mgr->active());
    if (scene_dirty) s.scene->mark_dirty();

    // The inspector draws script fields from live instances, so the editor
    // needs them back even though nothing ticks them.
    s.scripts->manager().deserialize_all(*s.scene);

    // Contexts pushed during play (play start, spawned InputComponents) are
    // never popped by anything else. Walk a copy: popping mutates the stack.
    const auto pushed = s.input_map->context_stack();
    for (const auto& name : pushed) {
        if (std::find(input_contexts.begin(), input_contexts.end(), name) == input_contexts.end()) {
            s.input_map->pop_context(name);
        }
    }

    for (const auto& name : s.vulkan->scene_viewport_names()) {
        if (std::find(viewports.begin(), viewports.end(), name) == viewports.end()) {
            s.vulkan->destroy_scene_viewport(name);
        }
    }

    s.screen_debug->clear();
    s.vulkan->sync_custom_depth_pipelines();
}

} // namespace fjell
