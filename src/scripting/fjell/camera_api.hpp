// camera_api.hpp — Static camera API for game scripts
#pragma once

#include <cstdint>

extern "C" {
void     fjell_camera_set_active(uint32_t node_id);
uint32_t fjell_camera_get_active();
float    fjell_camera_get_fov();
void     fjell_camera_set_fov(float fov);
float    fjell_camera_get_near();
void     fjell_camera_set_near(float near_plane);
float    fjell_camera_get_far();
void     fjell_camera_set_far(float far_plane);
void     fjell_camera_set_ortho(float size);
void     fjell_camera_set_perspective(float fov);
void     fjell_camera_set_projection_override(const float* mat4_ptr);
void     fjell_camera_clear_projection_override();
}

namespace fjell {

struct CameraAPI {
    static void set_active(uint32_t node_id) {
        fjell_camera_set_active(node_id);
    }
    static uint32_t active_id() {
        return fjell_camera_get_active();
    }
    static float fov() { return fjell_camera_get_fov(); }
    static void set_fov(float fov) { fjell_camera_set_fov(fov); }
    static float near_plane() { return fjell_camera_get_near(); }
    static void set_near(float v) { fjell_camera_set_near(v); }
    static float far_plane() { return fjell_camera_get_far(); }
    static void set_far(float v) { fjell_camera_set_far(v); }
    static void set_ortho(float size) { fjell_camera_set_ortho(size); }
    static void set_perspective(float fov) { fjell_camera_set_perspective(fov); }
    static void set_projection_override(const float* mat4_ptr) {
        fjell_camera_set_projection_override(mat4_ptr);
    }
    static void clear_projection_override() {
        fjell_camera_clear_projection_override();
    }
};

} // namespace fjell
