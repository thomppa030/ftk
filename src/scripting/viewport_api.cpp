#include "scripting/script_globals.hpp"
#include "scripting/fjell/viewport_api.hpp"
#include "renderer/vulkan_context.hpp"
#include "renderer/scene_viewport.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "scene/camera_node.hpp"
#include "scene/scene.hpp"

extern "C" uint32_t fjell_viewport_create(const char* name, uint32_t width, uint32_t height,
                                           uint32_t flags) {
    auto* vk = fjell::script_vulkan_context();
    if (!vk || !name) return 0;

    fjell::SceneViewportConfig cfg;
    cfg.name = name;
    cfg.width = width;
    cfg.height = height;
    cfg.pp_config.tonemap        = (flags & VP_TONEMAP) != 0;
    cfg.pp_config.bloom          = (flags & VP_BLOOM) != 0;
    cfg.pp_config.gtao           = (flags & VP_GTAO) != 0;
    cfg.pp_config.ssr            = (flags & VP_SSR) != 0;
    cfg.pp_config.volumetric_fog = (flags & VP_VOLUMETRIC_FOG) != 0;
    cfg.pp_config.hiz            = (flags & VP_HIZ) != 0;
    cfg.samples = vk->msaa_samples();
    cfg.skip_shadows = (flags & VP_SHADOWS) == 0;

    uint32_t bindless_idx = vk->create_scene_viewport(cfg);

    // Update any materials that were loaded with $viewport:name before this viewport existed
    auto* rm = fjell::script_resource_manager();
    if (rm) rm->update_viewport_bindings(name, bindless_idx);

    return bindless_idx;
}

extern "C" void fjell_viewport_destroy(const char* name) {
    auto* vk = fjell::script_vulkan_context();
    if (!vk || !name) return;

    // Reset material bindings back to default black before destroying the viewport
    auto* rm = fjell::script_resource_manager();
    if (rm) rm->update_viewport_bindings(name, rm->bindless_index(rm->default_white_handle()));

    vk->destroy_scene_viewport(name);
}

extern "C" void fjell_viewport_set_camera(const char* name, uint32_t camera_node_id) {
    auto* vk = fjell::script_vulkan_context();
    auto* scene = fjell::script_active_scene();
    if (!vk || !scene || !name) return;

    auto* viewport = vk->scene_viewport(name);
    if (!viewport) return;

    auto* node = scene->find(camera_node_id);
    auto* cam = dynamic_cast<fjell::CameraNode*>(node);
    if (!cam) return;

    float aspect = static_cast<float>(viewport->config().width)
                 / static_cast<float>(viewport->config().height);

    fjell::SceneViewportView view;
    view.view = cam->view_matrix();
    view.projection = cam->projection_matrix(aspect);
    view.camera_position = cam->position();
    viewport->set_view(view);
}

extern "C" uint32_t fjell_viewport_bindless_index(const char* name) {
    auto* vk = fjell::script_vulkan_context();
    if (!vk || !name) return 0;
    return vk->scene_viewport_bindless_index(name);
}

extern "C" void fjell_viewport_resize(const char* name, uint32_t width, uint32_t height) {
    auto* vk = fjell::script_vulkan_context();
    if (!vk || !name) return;
    auto* viewport = vk->scene_viewport(name);
    if (viewport) viewport->resize(width, height);
}
