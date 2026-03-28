#include "scripting/script_globals.hpp"
#include "scene/camera_node.hpp"
#include "scene/camera_subsystem.hpp"
#include "scene/scene.hpp"

#include <glm/glm.hpp>

static fjell::CameraNode* active_cam() {
    auto* sub = fjell::script_camera_subsystem();
    auto* scene = fjell::script_active_scene();
    if (!sub || !scene) return nullptr;
    return sub->active_camera(*scene);
}

extern "C" void fjell_camera_set_active(uint32_t node_id) {
    auto* sub = fjell::script_camera_subsystem();
    if (sub) sub->set_active(node_id);
}

extern "C" uint32_t fjell_camera_get_active() {
    auto* sub = fjell::script_camera_subsystem();
    return sub ? sub->active_id() : 0;
}

extern "C" float fjell_camera_get_fov() {
    auto* cam = active_cam();
    return cam ? cam->fov : 45.0f;
}

extern "C" void fjell_camera_set_fov(float fov) {
    auto* cam = active_cam();
    if (cam) cam->fov = fov;
}

extern "C" float fjell_camera_get_near() {
    auto* cam = active_cam();
    return cam ? cam->near_plane : 0.1f;
}

extern "C" void fjell_camera_set_near(float near_plane) {
    auto* cam = active_cam();
    if (cam) cam->near_plane = near_plane;
}

extern "C" float fjell_camera_get_far() {
    auto* cam = active_cam();
    return cam ? cam->far_plane : 1000.0f;
}

extern "C" void fjell_camera_set_far(float far_plane) {
    auto* cam = active_cam();
    if (cam) cam->far_plane = far_plane;
}

extern "C" void fjell_camera_set_ortho(float size) {
    auto* cam = active_cam();
    if (!cam) return;
    cam->projection = fjell::CameraProjection::orthographic;
    cam->ortho_size = size;
    cam->use_projection_override = false;
}

extern "C" void fjell_camera_set_perspective(float fov) {
    auto* cam = active_cam();
    if (!cam) return;
    cam->projection = fjell::CameraProjection::perspective;
    cam->fov = fov;
    cam->use_projection_override = false;
}

extern "C" void fjell_camera_set_projection_override(const float* mat4_ptr) {
    auto* cam = active_cam();
    if (!cam || !mat4_ptr) return;
    cam->use_projection_override = true;
    const auto* cols = reinterpret_cast<const glm::vec4*>(mat4_ptr);
    cam->projection_override = glm::mat4(cols[0], cols[1], cols[2], cols[3]);
}

extern "C" void fjell_camera_clear_projection_override() {
    auto* cam = active_cam();
    if (cam) cam->use_projection_override = false;
}
