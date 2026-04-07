#include "fjell/api.h"
#include "scripting/script_globals.hpp"
#include "scene/camera_node.hpp"
#include "scene/camera_subsystem.hpp"
#include "scene/scene.hpp"

#include <glm/glm.hpp>

static fjell::CameraNode* active_cam() {
    auto& s = fjell::script_services();
    if (!s.camera || !s.scene) return nullptr;
    return s.camera->active_camera(*s.scene);
}

extern "C" FJELL_SDK_EXPORT void fjell_camera_set_active(uint32_t node_id) {
    auto& s = fjell::script_services();
    if (s.camera) s.camera->set_active(node_id);
}

extern "C" FJELL_SDK_EXPORT uint32_t fjell_camera_get_active() {
    auto& s = fjell::script_services();
    return s.camera ? s.camera->active_id() : 0;
}

extern "C" FJELL_SDK_EXPORT float fjell_camera_get_fov() {
    auto* cam = active_cam();
    return cam ? cam->fov : 45.0f;
}

extern "C" FJELL_SDK_EXPORT void fjell_camera_set_fov(float fov) {
    auto* cam = active_cam();
    if (cam) cam->fov = fov;
}

extern "C" FJELL_SDK_EXPORT float fjell_camera_get_near() {
    auto* cam = active_cam();
    return cam ? cam->near_plane : 0.1f;
}

extern "C" FJELL_SDK_EXPORT void fjell_camera_set_near(float near_plane) {
    auto* cam = active_cam();
    if (cam) cam->near_plane = near_plane;
}

extern "C" FJELL_SDK_EXPORT float fjell_camera_get_far() {
    auto* cam = active_cam();
    return cam ? cam->far_plane : 1000.0f;
}

extern "C" FJELL_SDK_EXPORT void fjell_camera_set_far(float far_plane) {
    auto* cam = active_cam();
    if (cam) cam->far_plane = far_plane;
}

extern "C" FJELL_SDK_EXPORT void fjell_camera_set_ortho(float size) {
    auto* cam = active_cam();
    if (!cam) return;
    cam->projection = fjell::CameraProjection::orthographic;
    cam->ortho_size = size;
    cam->use_projection_override = false;
}

extern "C" FJELL_SDK_EXPORT void fjell_camera_set_perspective(float fov) {
    auto* cam = active_cam();
    if (!cam) return;
    cam->projection = fjell::CameraProjection::perspective;
    cam->fov = fov;
    cam->use_projection_override = false;
}

extern "C" FJELL_SDK_EXPORT void fjell_camera_set_projection_override(const float* mat4_ptr) {
    auto* cam = active_cam();
    if (!cam || !mat4_ptr) return;
    cam->use_projection_override = true;
    const auto* cols = reinterpret_cast<const glm::vec4*>(mat4_ptr);
    cam->projection_override = glm::mat4(cols[0], cols[1], cols[2], cols[3]);
}

extern "C" FJELL_SDK_EXPORT void fjell_camera_clear_projection_override() {
    auto* cam = active_cam();
    if (cam) cam->use_projection_override = false;
}
