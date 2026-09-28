// camera_api.hpp — The scene's active camera, for game scripts
#pragma once

#include "fjell/api.h"
#include "fjell/ref.hpp"

#include <cstdint>

extern "C" {
FJELL_SDK_EXPORT void     fjell_camera_set_active(uint32_t node_id);
FJELL_SDK_EXPORT uint32_t fjell_camera_get_active();
FJELL_SDK_EXPORT float    fjell_camera_get_fov();
FJELL_SDK_EXPORT void     fjell_camera_set_fov(float fov);
FJELL_SDK_EXPORT float    fjell_camera_get_near();
FJELL_SDK_EXPORT void     fjell_camera_set_near(float near_plane);
FJELL_SDK_EXPORT float    fjell_camera_get_far();
FJELL_SDK_EXPORT void     fjell_camera_set_far(float far_plane);
FJELL_SDK_EXPORT void     fjell_camera_set_ortho(float size);
FJELL_SDK_EXPORT void     fjell_camera_set_perspective(float fov);
FJELL_SDK_EXPORT void     fjell_camera_set_projection_override(const float* mat4_ptr);
FJELL_SDK_EXPORT void     fjell_camera_clear_projection_override();
}

namespace fjell {

/// Controls the scene's active camera. Every getter and setter here targets
/// whichever camera is currently active — not a specific node — so
/// `set_active()` first, then adjust.
///
/// Setters silently do nothing when no camera is active, and getters fall back
/// to defaults (fov 45, near 0.1, far 1000) rather than reporting an error.
///
/// @code
/// fjell::camera::set_active(self().find_child("Camera"));
/// fjell::camera::set_fov(70.0f);
/// @endcode
namespace camera {
/// Render the scene from `camera` from now on. A camera that is gone stops
/// the game (see `Ref`); a node that is no camera is reported and leaves the
/// active camera as it is.
inline void set_active(const Ref<Node3D>& camera) {
    detail::require(camera);
    fjell_camera_set_active(camera.node_id());
}

/// The camera the scene renders from, or an empty Ref when none is active.
inline Ref<Node3D> active() {
    return Ref<Node3D>{fjell_camera_get_active()};
}

/// Vertical field of view in degrees. Returns 45 if no camera is active.
/// Meaningful only in perspective mode.
inline float fov() { return fjell_camera_get_fov(); }

/// Set vertical field of view in degrees. Does not switch a camera out of
/// orthographic mode — use `set_perspective()` for that.
inline void set_fov(float fov) { fjell_camera_set_fov(fov); }

/// Near clip distance. Returns 0.1 if no camera is active.
inline float near_plane() { return fjell_camera_get_near(); }

/// Set the near clip distance. Keep this as large as the game tolerates:
/// very small values (below ~0.01) burn depth-buffer precision and cause
/// banding in depth-based effects such as ambient occlusion.
inline void set_near(float v) { fjell_camera_set_near(v); }

/// Far clip distance. Returns 1000 if no camera is active.
inline float far_plane() { return fjell_camera_get_far(); }

/// Set the far clip distance. Geometry beyond it is not drawn.
inline void set_far(float v) { fjell_camera_set_far(v); }

/// Switch the active camera to an orthographic projection.
/// @param size Vertical extent of the view volume in world units.
/// Clears any projection override.
inline void set_ortho(float size) { fjell_camera_set_ortho(size); }

/// Switch the active camera to a perspective projection.
/// @param fov Vertical field of view in degrees.
/// Clears any projection override.
inline void set_perspective(float fov) { fjell_camera_set_perspective(fov); }

/// Replace the computed projection with a custom matrix, for cases the
/// built-in modes don't cover (oblique near-plane clipping, sheared
/// frusta). Overrides fov/near/far until cleared.
/// @param mat4_ptr Pointer to 16 floats in column-major order, e.g.
///                 `glm::value_ptr(my_matrix)`. Must not be null.
inline void set_projection_override(const float* mat4_ptr) {
    fjell_camera_set_projection_override(mat4_ptr);
}

/// Drop a projection override and return to the camera's own
/// perspective/orthographic settings.
inline void clear_projection_override() {
    fjell_camera_clear_projection_override();
}
} // namespace camera

} // namespace fjell
