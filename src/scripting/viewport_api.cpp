#include "fjell/api.h"
#include "scripting/script_globals.hpp"
#include "scripting/fjell/viewport_api.hpp"
#include "renderer/fjgraph_asset.hpp"
#include "renderer/post_process_resources.hpp"
#include "renderer/scene_viewport.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/vulkan_context.hpp"
#include "scene/camera_node.hpp"
#include "scene/scene.hpp"

extern "C" FJELL_SDK_EXPORT uint32_t fjell_viewport_create(const char* name, uint32_t width, uint32_t height,
                                           uint32_t flags) {
    auto& s = fjell::script_services();
    if (!s.vulkan || !name) return 0;

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
    cfg.samples = s.vulkan->msaa_samples();
    cfg.skip_shadows = (flags & VP_SHADOWS) == 0;
    // Legacy path: cfg.viewport_graph stays null. RenderPipeline::execute
    // falls back to global pass_state_ flags for any viewport that
    // doesn't supply a graph.

    uint32_t bindless_idx = s.vulkan->create_scene_viewport(cfg);

    if (s.resources) s.resources->update_viewport_bindings(name, bindless_idx);

    return bindless_idx;
}

extern "C" FJELL_SDK_EXPORT uint32_t fjell_viewport_create_with_graph(
        const char* name, uint32_t width, uint32_t height, const char* graph_path) {
    auto& s = fjell::script_services();
    if ((s.vulkan == nullptr) || (name == nullptr) || (graph_path == nullptr)) {
        return 0;
    }

    const fjell::FjGraph* graph = s.vulkan->get_or_load_graph(graph_path);
    if (graph == nullptr) { return 0; } // load failure already logged

    fjell::SceneViewportConfig cfg;
    cfg.name = name;
    cfg.width = width;
    cfg.height = height;
    cfg.pp_config = fjell::pp_config_from_graph(*graph);
    cfg.samples = s.vulkan->msaa_samples();
    cfg.viewport_graph = graph;
    // Skip shadows by default — the graph's ShadowPass entry decides
    // whether shadows render at all, but `skip_shadows` is the per-viewport
    // hint that the main viewport's shadow data is reused (cheap path).
    // Authors can override later via a separate skip_shadows knob.
    cfg.skip_shadows = true;

    uint32_t bindless_idx = s.vulkan->create_scene_viewport(cfg);

    if (s.resources) { s.resources->update_viewport_bindings(name, bindless_idx); }

    return bindless_idx;
}

extern "C" FJELL_SDK_EXPORT void fjell_viewport_destroy(const char* name) {
    auto& s = fjell::script_services();
    if (!s.vulkan || !name) return;

    if (s.resources) s.resources->update_viewport_bindings(name,
        s.resources->bindless_index(s.resources->default_white_handle()));

    s.vulkan->destroy_scene_viewport(name);
}

extern "C" FJELL_SDK_EXPORT void fjell_viewport_set_camera(const char* name, uint32_t camera_node_id) {
    auto& s = fjell::script_services();
    if (!s.vulkan || !s.scene || !name) return;

    auto* viewport = s.vulkan->scene_viewport(name);
    if (!viewport) return;

    auto* node = s.scene->find(camera_node_id);
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

extern "C" FJELL_SDK_EXPORT uint32_t fjell_viewport_bindless_index(const char* name) {
    auto& s = fjell::script_services();
    if (!s.vulkan || !name) return 0;
    return s.vulkan->scene_viewport_bindless_index(name);
}

extern "C" FJELL_SDK_EXPORT void fjell_viewport_resize(const char* name, uint32_t width, uint32_t height) {
    auto& s = fjell::script_services();
    if (!s.vulkan || !name) return;
    auto* viewport = s.vulkan->scene_viewport(name);
    if (viewport) viewport->resize(width, height);
}
