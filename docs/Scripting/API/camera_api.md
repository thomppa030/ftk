# fjell::camera

Controls the active rendering camera and its projection.

| | |
|---|---|
| Header | `fjell/camera_api.hpp` |
| Source | `src/scripting/camera_api.cpp` |

## Syntax

```cpp
namespace fjell {
namespace camera {
    void     set_active(uint32_t node_id);
    uint32_t active_id();

    float fov();
    void  set_fov(float fov);
    float near_plane();
    void  set_near(float near_plane);
    float far_plane();
    void  set_far(float far_plane);

    void  set_ortho(float size);
    void  set_perspective(float fov);

    void  set_projection_override(const float* mat4_ptr);
    void  clear_projection_override();
}
}
```

## Remarks

The "active camera" is the one used to render the main swapchain. Render-to-texture viewports (see [`viewport`](viewport_api.md)) have their own per-viewport cameras and are unaffected by these functions.

Camera state lives on a `CameraComponent` attached to a scene node. The setters here modify whichever camera is currently active. `set_active(id)` switches to a different camera; its own settings (FOV, near/far) come along.

`set_ortho` and `set_perspective` switch projection mode and set the primary parameter in one call. `set_projection_override` lets you provide a fully custom 4×4 projection matrix that bypasses the standard parameter system.

## Functions

| Name | Description |
|---|---|
| [`set_active`](#cameraset_active) | Switch the active camera |
| [`active_id`](#cameraactive_id) | Get the active camera's node ID |
| [`fov`](#camerafov) | Get current vertical FOV (degrees) |
| [`set_fov`](#cameraset_fov) | Set vertical FOV (degrees) |
| [`near_plane`](#cameranear_plane-far_plane-set_near-set_far) | Get near clip distance |
| [`set_near`](#cameranear_plane-far_plane-set_near-set_far) | Set near clip distance |
| [`far_plane`](#cameranear_plane-far_plane-set_near-set_far) | Get far clip distance |
| [`set_far`](#cameranear_plane-far_plane-set_near-set_far) | Set far clip distance |
| [`set_ortho`](#cameraset_ortho) | Switch to orthographic projection |
| [`set_perspective`](#cameraset_perspective) | Switch to perspective projection |
| [`set_projection_override`](#cameraset_projection_override) | Install a custom 4×4 projection matrix |
| [`clear_projection_override`](#cameraclear_projection_override) | Remove the override |

---

## camera::set_active

### Syntax
```cpp
void set_active(uint32_t node_id);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `node_id` | `uint32_t` | ID of a node carrying a `CameraComponent`. |

### Remarks
Takes effect on the next frame. Passing an ID that doesn't have a camera component is a no-op (the previous active camera remains).

---

## camera::active_id

### Syntax
```cpp
[[nodiscard]] uint32_t active_id();
```

### Return value
The node ID of the currently active camera, or `0` if none is set.

---

## camera::fov

### Syntax
```cpp
[[nodiscard]] float fov();
```

### Return value
The active camera's vertical field of view in **degrees**.

### Remarks
Meaningful only in perspective mode. Returns the stored value regardless of current projection mode.

---

## camera::set_fov

### Syntax
```cpp
void set_fov(float fov);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `fov` | `float` | Vertical field of view in degrees. Typical range 30..120. |

### Remarks
Doesn't switch projection mode — use `set_perspective` for that. Sane values: 60–75 for first-person, 40–60 for third-person, 90+ for wide / VR-style.

---

## camera::near_plane / far_plane / set_near / set_far

### Syntax
```cpp
[[nodiscard]] float near_plane();
[[nodiscard]] float far_plane();
void  set_near(float near_plane);
void  set_far (float far_plane);
```

### Remarks
World-space distances. The ratio `far / near` drives depth-buffer precision — keep `near` as large as the scene allows.

---

## camera::set_ortho

### Syntax
```cpp
void set_ortho(float size);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `size` | `float` | Orthographic half-height in world units. Width is derived from the viewport aspect ratio. |

### Remarks
Switches the active camera to orthographic projection and sets its size in one call.

---

## camera::set_perspective

### Syntax
```cpp
void set_perspective(float fov);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `fov` | `float` | Vertical FOV in degrees. |

### Remarks
Switches the active camera to perspective projection and sets its FOV in one call.

---

## camera::set_projection_override

### Syntax
```cpp
void set_projection_override(const float* mat4_ptr);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `mat4_ptr` | `const float*` | Pointer to 16 floats in **column-major** order (GLM `mat4` layout). Must not be null. Must outlive the call (engine copies internally). |

### Remarks
Bypasses FOV / near / far / mode and uses the provided matrix as the projection. Useful for portals, oblique frustums, custom asymmetric setups.

### Example
```cpp
glm::mat4 m = glm::perspectiveLH(/*...*/);
fjell::camera::set_projection_override(&m[0][0]);
```

---

## camera::clear_projection_override

### Syntax
```cpp
void clear_projection_override();
```

### Remarks
Restores normal projection driven by mode + FOV + near + far.

## Example

Switch cameras for a cinematic:

```cpp
void play_cutscene() {
    saved_cam_ = fjell::camera::active_id();
    uint32_t cine = fjell::scene::find_node("CinematicCam");
    fjell::camera::set_active(cine);
}

void end_cutscene() {
    fjell::camera::set_active(saved_cam_);
}
```

## See also
- [`scene::find_node`](scene_api.md#scenefind_node) — finding the camera node
- [`viewport`](viewport_api.md) — render-to-texture from other cameras
