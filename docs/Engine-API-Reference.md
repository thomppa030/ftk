# Engine API Reference

The `FjellEngineApi` struct is passed to component callbacks. It provides access to engine systems from game code.

Defined in `src/scripting/fjell_api.h`.

## Context

```c
uint32_t object_id;     // ID of the object this component is attached to
```

## Transform

```c
void (*get_position)(uint32_t id, float* out_xyz);
void (*set_position)(uint32_t id, const float* xyz);
void (*get_rotation)(uint32_t id, float* out_xyz);   // Euler degrees (XYZ)
void (*set_rotation)(uint32_t id, const float* xyz);
void (*get_scale)(uint32_t id, float* out_xyz);
void (*set_scale)(uint32_t id, const float* xyz);
```

All transform functions take an object ID — you can read/write transforms of any object in the scene, not just the current one. Rotations are Euler angles in degrees, XYZ order.

## Input

```c
uint8_t (*is_key_held)(int key);          // True while key is down
uint8_t (*is_key_pressed)(int key);       // True for one frame on press
uint8_t (*is_key_released)(int key);      // True for one frame on release
uint8_t (*is_mouse_button_held)(int button);
uint8_t (*is_mouse_button_pressed)(int button);
void    (*get_mouse_delta)(float* out_xy);
```

Key codes are GLFW key constants (e.g., `GLFW_KEY_W = 87`, `GLFW_KEY_SPACE = 32`). Mouse buttons: `0 = left`, `1 = right`, `2 = middle`.

During play mode the cursor is captured, so `get_mouse_delta` returns frame-to-frame movement.

## Time

```c
float delta_time;       // Seconds since last frame
float elapsed_time;     // Total seconds since engine start
```

## Logging

```c
void (*log_info)(const char* msg);
void (*log_warn)(const char* msg);
void (*log_error)(const char* msg);
```

Messages appear in the in-editor console panel and stdout, prefixed with `[script]`.

## Components

```c
void* (*get_component)(uint32_t object_id, const char* type_name);
```

Returns a pointer to the raw component data for the given type on the given object, or null if the object doesn't have that component. Use it to communicate between components:

```c
// C style
Health* health = (Health*)api->get_component(api->object_id, "Health");
if (health) health->hp -= 10;
```

With the C++ wrapper (`fjell.hpp`):

```cpp
// Same object
if (auto* health = fjell::get<Health>(api))
    health->hp -= 10;

// Different object
if (auto* health = fjell::get<Health>(api, other_id))
    health->hp -= 10;
```

## Physics

All physics functions are null-safe — they no-op silently if physics isn't active (outside play mode).

### Forces and Impulses

```c
void (*apply_force)(uint32_t id, const float* xyz);
void (*apply_impulse)(uint32_t id, const float* xyz);
```

`apply_force` applies a continuous force (integrate over dt — call every frame). `apply_impulse` applies an instantaneous velocity change (e.g., jump). Both take world-space vectors.

### Velocity

```c
void (*set_linear_velocity)(uint32_t id, const float* xyz);
void (*get_linear_velocity)(uint32_t id, float* out_xyz);
void (*set_angular_velocity)(uint32_t id, const float* xyz);
void (*get_angular_velocity)(uint32_t id, float* out_xyz);
```

Direct velocity control. Angular velocity is in radians/second around each axis.

### Raycast

```c
uint8_t (*raycast)(const float* origin,       // World-space ray start
                   const float* direction,     // Ray direction (normalized)
                   float max_distance,         // Maximum ray length
                   float* out_hit_pos,         // Hit point (nullable)
                   float* out_hit_normal,      // Surface normal at hit (nullable)
                   float* out_distance,        // Distance to hit (nullable)
                   uint32_t* out_object_id);   // Object that was hit (nullable)
```

Returns 1 if something was hit, 0 otherwise. Output pointers can be null if you don't need that info.

## JSON Bridge

For custom serialization callbacks:

### Writing

```c
void fjell_json_write_float(FjellJsonWriter* w, const char* key, float v);
void fjell_json_write_int(FjellJsonWriter* w, const char* key, int32_t v);
void fjell_json_write_bool(FjellJsonWriter* w, const char* key, uint8_t v);
void fjell_json_write_vec2(FjellJsonWriter* w, const char* key, const float* xy);
void fjell_json_write_vec3(FjellJsonWriter* w, const char* key, const float* xyz);
void fjell_json_write_vec4(FjellJsonWriter* w, const char* key, const float* xyzw);
void fjell_json_write_string(FjellJsonWriter* w, const char* key, const char* v);
```

### Reading

```c
float    fjell_json_read_float(const FjellJsonReader* r, const char* key, float fallback);
int32_t  fjell_json_read_int(const FjellJsonReader* r, const char* key, int32_t fallback);
uint8_t  fjell_json_read_bool(const FjellJsonReader* r, const char* key, uint8_t fallback);
void     fjell_json_read_vec2(const FjellJsonReader* r, const char* key, float* out, const float* fallback);
void     fjell_json_read_vec3(const FjellJsonReader* r, const char* key, float* out, const float* fallback);
void     fjell_json_read_vec4(const FjellJsonReader* r, const char* key, float* out, const float* fallback);
const char* fjell_json_read_string(const FjellJsonReader* r, const char* key, const char* fallback);
```

All read functions take a fallback value returned when the key is missing.
